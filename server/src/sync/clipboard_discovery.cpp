#include "sync/clipboard_discovery.h"
#include "sync/clipboard_tls.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <chrono>
#include <map>
#include <set>
#include <utility>

namespace
{
using Clock = std::chrono::steady_clock;
#ifdef _WIN32
using SocketValue = SOCKET;
constexpr SocketValue kInvalid = INVALID_SOCKET;
#else
using SocketValue = int;
constexpr SocketValue kInvalid = -1;
#endif

bool NetworkReady()
{
#ifdef _WIN32
    static const bool ready = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ready;
#else
    return true;
#endif
}

std::string Mac(std::string_view body, const std::string &secret)
{
    if (!MsimeClipboardLan::IsHex(secret, MsimeClipboardLan::kSecretBytes))
        return {};
    unsigned char key[MsimeClipboardLan::kSecretBytes]{};
    auto nibble = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; i < sizeof(key); ++i)
        key[i] = static_cast<unsigned char>((nibble(secret[i * 2]) << 4) | nibble(secret[i * 2 + 1]));
    std::string tag(MsimeClipboardLan::kDiscoveryTagBytes, '\0');
    size_t written = 0;
    const bool ok = EVP_Q_mac(nullptr, "HMAC", nullptr, "SHA256", nullptr, key, sizeof(key),
                              reinterpret_cast<const unsigned char *>(body.data()), body.size(),
                              reinterpret_cast<unsigned char *>(tag.data()), tag.size(), &written) &&
                    written == tag.size();
    OPENSSL_cleanse(key, sizeof(key));
    return ok ? tag : std::string{};
}

bool SendPacket(SocketValue socket, const ClipboardSync::DiscoveryDestination &target, std::string_view packet)
{
    if (!target.port || !ClipboardSync::IsLanAddress(target.address) || packet.empty())
        return false;
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(target.port);
    if (inet_pton(AF_INET, target.address.c_str(), &destination.sin_addr) != 1)
        return false;
    return sendto(socket, packet.data(), static_cast<int>(packet.size()), 0,
                  reinterpret_cast<const sockaddr *>(&destination),
                  sizeof(destination)) == static_cast<int>(packet.size());
}
} // namespace

std::string ClipboardSync::SignDiscovery(const MsimeClipboardLan::DiscoveryMessage &message, const std::string &secret)
{
    const auto body = MsimeClipboardLan::DiscoveryBody(message);
    if (body.empty())
        return {};
    const auto tag = Mac(body, secret);
    return tag.empty() ? std::string{} : body + tag;
}

bool ClipboardSync::VerifyDiscovery(std::string_view packet, const std::string &secret,
                                    MsimeClipboardLan::DiscoveryMessage &message)
{
    message = {};
    if (packet.size() < MsimeClipboardLan::kDiscoveryTagBytes || packet.size() > MsimeClipboardLan::kDiscoveryMaxBytes)
        return false;
    const auto body = packet.substr(0, packet.size() - MsimeClipboardLan::kDiscoveryTagBytes);
    MsimeClipboardLan::DiscoveryMessage candidate;
    if (!MsimeClipboardLan::DecodeDiscoveryBody(body, candidate))
        return false;
    const auto tag = Mac(body, secret);
    if (tag.empty() || CRYPTO_memcmp(tag.data(), packet.data() + body.size(), tag.size()) != 0)
        return false;
    message = std::move(candidate);
    return true;
}

std::vector<ClipboardSync::DiscoveryDestination> ClipboardSync::LanBroadcasts(unsigned short port)
{
    std::set<std::string> addresses;
    auto add = [&](uint32_t address, unsigned int prefix) {
        in_addr local{};
        local.s_addr = htonl(address);
        char text[INET_ADDRSTRLEN]{};
        if (!prefix || prefix >= 31 || !inet_ntop(AF_INET, &local, text, sizeof(text)) || !IsLanAddress(text) ||
            (address >> 24) == 127)
            return;
        in_addr broadcast{};
        broadcast.s_addr = htonl(address | (0xffffffffu >> prefix));
        if (inet_ntop(AF_INET, &broadcast, text, sizeof(text)) && IsLanAddress(text))
            addresses.insert(text);
    };
#ifdef _WIN32
    ULONG size = 16384;
    std::vector<unsigned char> buffer(size);
    auto *adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
    ULONG result = GetAdaptersAddresses(
        AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters, &size);
    if (result == ERROR_BUFFER_OVERFLOW && size <= 1024 * 1024)
    {
        buffer.resize(size);
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data());
        result =
            GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, adapters, &size);
    }
    if (result == NO_ERROR)
        for (auto *adapter = adapters; adapter; adapter = adapter->Next)
            if (adapter->OperStatus == IfOperStatusUp)
                for (auto *unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next)
                    if (unicast->Address.lpSockaddr && unicast->Address.lpSockaddr->sa_family == AF_INET)
                        add(ntohl(reinterpret_cast<sockaddr_in *>(unicast->Address.lpSockaddr)->sin_addr.s_addr),
                            unicast->OnLinkPrefixLength);
#else
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0)
    {
        for (auto *item = interfaces; item; item = item->ifa_next)
            if (item->ifa_addr && item->ifa_netmask && item->ifa_addr->sa_family == AF_INET &&
                (item->ifa_flags & IFF_UP) && (item->ifa_flags & IFF_BROADCAST))
            {
                const uint32_t mask = ntohl(reinterpret_cast<sockaddr_in *>(item->ifa_netmask)->sin_addr.s_addr);
                unsigned int prefix = 0;
                while (prefix < 32 && (mask & (0x80000000u >> prefix)))
                    ++prefix;
                add(ntohl(reinterpret_cast<sockaddr_in *>(item->ifa_addr)->sin_addr.s_addr), prefix);
            }
        freeifaddrs(interfaces);
    }
#endif
    std::vector<DiscoveryDestination> targets;
    for (const auto &address : addresses)
    {
        if (targets.size() == 16)
            break;
        targets.push_back({address, port});
    }
    return targets;
}

struct ClipboardSync::LanDiscovery::Impl
{
    struct Pending
    {
        MsimeClipboardLan::PairingCode peer;
        std::string nonce;
        Clock::time_point expires;
    };
    SocketValue socket = kInvalid;
    std::string local;
    unsigned short udp_port = 0, tcp_port = 0;
    std::map<std::string, Pending> pending;
    std::map<std::string, Clock::time_point> reverse_probes;
    ~Impl()
    {
        if (socket != kInvalid)
        {
#ifdef _WIN32
            closesocket(socket);
#else
            close(socket);
#endif
        }
    }
};

ClipboardSync::LanDiscovery::LanDiscovery(std::string local_device) : impl_(std::make_unique<Impl>())
{
    impl_->local = std::move(local_device);
}
ClipboardSync::LanDiscovery::~LanDiscovery() = default;

bool ClipboardSync::LanDiscovery::Open(const std::string &address, unsigned short discovery_port,
                                       unsigned short tcp_port)
{
    if (!NetworkReady() || impl_->socket != kInvalid || !tcp_port || !MsimeClipboardSync::ValidIdentity(impl_->local) ||
        (address != "0.0.0.0" && !IsLanAddress(address)))
        return false;
    impl_->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == kInvalid)
        return false;
    int enabled = 1;
    if (setsockopt(impl_->socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char *>(&enabled),
                   sizeof(enabled)) != 0)
        return false;
#ifdef _WIN32
    u_long nonblocking = 1;
    if (ioctlsocket(impl_->socket, FIONBIO, &nonblocking) != 0 ||
        setsockopt(impl_->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&enabled),
                   sizeof(enabled)) != 0)
        return false;
#else
    if (fcntl(impl_->socket, F_SETFL, O_NONBLOCK) != 0)
        return false;
#endif
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(discovery_port);
    if (inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1 ||
        bind(impl_->socket, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) != 0)
        return false;
#ifdef _WIN32
    int length = sizeof(endpoint);
#else
    socklen_t length = sizeof(endpoint);
#endif
    if (getsockname(impl_->socket, reinterpret_cast<sockaddr *>(&endpoint), &length) != 0)
        return false;
    impl_->udp_port = ntohs(endpoint.sin_port);
    impl_->tcp_port = tcp_port;
    return true;
}

unsigned short ClipboardSync::LanDiscovery::Port() const
{
    return impl_->udp_port;
}

bool ClipboardSync::LanDiscovery::Request(const MsimeClipboardLan::PairingCode &peer,
                                          const std::vector<DiscoveryDestination> &targets)
{
    const auto now = Clock::now();
    for (auto it = impl_->pending.begin(); it != impl_->pending.end();)
        if (it->second.expires <= now)
            it = impl_->pending.erase(it);
        else
            ++it;
    if (!impl_->udp_port || targets.empty() || targets.size() > 17 || peer.device == impl_->local ||
        MsimeClipboardLan::EncodePairingCode(peer).empty() ||
        (impl_->pending.size() >= MsimeClipboardLan::kMaxPeers + 1 && !impl_->pending.count(peer.key_id)))
        return false;
    const auto nonce = RandomHex(MsimeClipboardLan::kKeyIdBytes);
    const auto packet = SignDiscovery(
        {MsimeClipboardLan::DiscoveryType::Request, nonce, peer.key_id, impl_->local, peer.device, impl_->tcp_port},
        peer.secret);
    if (packet.empty())
        return false;
    bool sent = false;
    for (const auto &target : targets)
        sent = SendPacket(impl_->socket, target, packet) || sent;
    if (sent)
        impl_->pending[peer.key_id] = {peer, nonce, now + std::chrono::seconds(8)};
    return sent;
}

std::vector<ClipboardSync::DiscoveredPeer> ClipboardSync::LanDiscovery::Poll(const DiscoveryLookup &lookup,
                                                                             const std::atomic<bool> &stop)
{
    std::vector<DiscoveredPeer> found;
    if (!impl_->udp_port || !lookup || stop)
        return found;
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(impl_->socket, &descriptors);
    timeval interval{0, 50000};
    if (select(static_cast<int>(impl_->socket + 1), &descriptors, nullptr, nullptr, &interval) <= 0 || stop)
        return found;
    // Bound work and packet allocations even when an untrusted host floods UDP.
    for (unsigned count = 0; count < 16 && !stop; ++count)
    {
        char packet[MsimeClipboardLan::kDiscoveryMaxBytes + 1]{};
        sockaddr_in source{};
#ifdef _WIN32
        int length = sizeof(source);
#else
        socklen_t length = sizeof(source);
#endif
        const auto received =
            recvfrom(impl_->socket, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&source), &length);
        if (received < 0)
            break;
        char address[INET_ADDRSTRLEN]{};
        MsimeClipboardLan::DiscoveryMessage message;
        if (static_cast<size_t>(received) <= MsimeClipboardLan::kDiscoveryTagBytes ||
            static_cast<size_t>(received) > MsimeClipboardLan::kDiscoveryMaxBytes ||
            !inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address)) || !IsLanAddress(address) ||
            !MsimeClipboardLan::DecodeDiscoveryBody(
                std::string_view(packet, static_cast<size_t>(received) - MsimeClipboardLan::kDiscoveryTagBytes),
                message) ||
            message.to != impl_->local)
            continue;
        const auto secret = lookup(message.key_id, message.from);
        if (!secret || !VerifyDiscovery(std::string_view(packet, received), *secret, message))
            continue;
        const auto now = Clock::now();
        if (message.type == MsimeClipboardLan::DiscoveryType::Reply)
        {
            const auto pending = impl_->pending.find(message.key_id);
            if (pending == impl_->pending.end() || pending->second.expires <= now ||
                pending->second.nonce != message.nonce || pending->second.peer.device != message.from ||
                pending->second.peer.secret != *secret)
                continue;
            found.push_back({message.from, message.key_id, address, message.port});
            impl_->pending.erase(pending); // Reject unsolicited and replayed replies.
        }
        else
        {
            const DiscoveryDestination target{address, ntohs(source.sin_port)};
            SendPacket(impl_->socket, target,
                       SignDiscovery({MsimeClipboardLan::DiscoveryType::Reply, message.nonce, message.key_id,
                                      impl_->local, message.from, impl_->tcp_port},
                                     *secret));
            // A request's source address is not accepted as a fresh endpoint. Probe
            // it with our own nonce so either device can recover after changing IP.
            for (auto it = impl_->reverse_probes.begin(); it != impl_->reverse_probes.end();)
                if (now - it->second >= std::chrono::seconds(1))
                    it = impl_->reverse_probes.erase(it);
                else
                    ++it;
            // A stale pending broadcast/unicast must not block probing a peer's
            // new address. Replace its nonce, with at most one reverse probe/sec.
            if (!impl_->reverse_probes.count(message.key_id) &&
                impl_->reverse_probes.size() < MsimeClipboardLan::kMaxPeers + 1 &&
                Request({message.from, message.key_id, *secret}, {target}))
                impl_->reverse_probes[message.key_id] = now;
        }
    }
    return found;
}
