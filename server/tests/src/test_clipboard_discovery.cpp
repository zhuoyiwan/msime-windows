#include "sync/clipboard_discovery.h"
#include "sync/clipboard_tls.h"
#include "tests/includes/test_framework.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>

namespace
{
const MsimeClipboardLan::PairingCode key{"host", std::string(32, '1'), std::string(64, '2')};

ClipboardSync::DiscoveryLookup Lookup(std::string allowed)
{
    return [allowed](const std::string &id, const std::string &device) -> std::optional<std::string> {
        return id == key.key_id && device == allowed ? std::optional<std::string>(key.secret) : std::nullopt;
    };
}

struct RawPeer
{
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
#else
    int socket = -1;
#endif
    sockaddr_in requester{};
    unsigned short port = 0;
    RawPeer()
    {
        // LanDiscovery::Open initializes Winsock before this fixture is constructed.
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
        REQUIRE(bind(socket, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == 0);
#ifdef _WIN32
        int length = sizeof(endpoint);
#else
        socklen_t length = sizeof(endpoint);
#endif
        REQUIRE(getsockname(socket, reinterpret_cast<sockaddr *>(&endpoint), &length) == 0);
        port = ntohs(endpoint.sin_port);
    }
    ~RawPeer()
    {
#ifdef _WIN32
        closesocket(socket);
#else
        close(socket);
#endif
    }
    std::string Receive()
    {
        fd_set descriptors;
        FD_ZERO(&descriptors);
        FD_SET(socket, &descriptors);
        timeval timeout{1, 0};
        REQUIRE(select(static_cast<int>(socket + 1), &descriptors, nullptr, nullptr, &timeout) == 1);
        char packet[512]{};
#ifdef _WIN32
        int length = sizeof(requester);
#else
        socklen_t length = sizeof(requester);
#endif
        const auto count =
            recvfrom(socket, packet, sizeof(packet), 0, reinterpret_cast<sockaddr *>(&requester), &length);
        REQUIRE(count > 0);
        return std::string(packet, count);
    }
    void Reply(const std::string &packet)
    {
        REQUIRE(sendto(socket, packet.data(), static_cast<int>(packet.size()), 0,
                       reinterpret_cast<const sockaddr *>(&requester),
                       sizeof(requester)) == static_cast<int>(packet.size()));
    }
};
} // namespace

TEST_CASE(clipboard_discovery_authenticated_codec_rejects_tampering_and_noncanonical_packets)
{
    const MsimeClipboardLan::DiscoveryMessage source{
        MsimeClipboardLan::DiscoveryType::Request, std::string(32, '0'), key.key_id, "client", "host", 49876};
    const auto packet = ClipboardSync::SignDiscovery(source, key.secret);
    REQUIRE(!packet.empty());
    // Independently generated with Python hmac/hashlib for other host implementations.
    std::string tag_hex;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char byte : packet.substr(packet.size() - MsimeClipboardLan::kDiscoveryTagBytes))
    {
        tag_hex.push_back(hex[byte >> 4]);
        tag_hex.push_back(hex[byte & 15]);
    }
    REQUIRE_EQ(tag_hex, std::string("f99ca891dfc6738a2337dab2ee346c8db8d504682bf4ae75b96dbeceff146fd8"));
    REQUIRE(packet.find(key.secret) == std::string::npos);
    MsimeClipboardLan::DiscoveryMessage decoded;
    REQUIRE(ClipboardSync::VerifyDiscovery(packet, key.secret, decoded));
    REQUIRE_EQ(decoded.from, std::string("client"));
    REQUIRE_EQ(decoded.to, std::string("host"));
    REQUIRE_EQ(decoded.port, 49876);
    REQUIRE(!ClipboardSync::VerifyDiscovery(packet, std::string(64, '3'), decoded));
    for (size_t index = 0; index < packet.size(); ++index)
    {
        auto altered = packet;
        altered[index] ^= 1;
        REQUIRE(!ClipboardSync::VerifyDiscovery(altered, key.secret, decoded));
        REQUIRE(!ClipboardSync::VerifyDiscovery(std::string_view(packet).substr(0, index), key.secret, decoded));
    }
    REQUIRE(!ClipboardSync::VerifyDiscovery(packet + 'x', key.secret, decoded));
    auto invalid = source;
    invalid.to = invalid.from;
    REQUIRE(ClipboardSync::SignDiscovery(invalid, key.secret).empty());
    invalid = source;
    invalid.port = 0;
    REQUIRE(ClipboardSync::SignDiscovery(invalid, key.secret).empty());
}

TEST_CASE(clipboard_discovery_locates_both_devices_and_a_changed_ip_with_same_identity)
{
    std::atomic<bool> stop{false};
    ClipboardSync::LanDiscovery client("client"), host("host");
    REQUIRE(client.Open("127.0.0.1", 0, 1111));
    REQUIRE(host.Open("127.0.0.2", 0, 2222));
    REQUIRE(client.Request(key, {{"127.0.0.2", host.Port()}}));
    REQUIRE(host.Poll(Lookup("client"), stop).empty());
    const auto found = client.Poll(Lookup("host"), stop);
    REQUIRE_EQ(found.size(), size_t{1});
    REQUIRE_EQ(found.front().device, std::string("host"));
    REQUIRE_EQ(found.front().address, std::string("127.0.0.2"));
    REQUIRE_EQ(found.front().port, 2222);
    const auto reverse = host.Poll(Lookup("client"), stop);
    REQUIRE_EQ(reverse.size(), size_t{1});
    REQUIRE_EQ(reverse.front().device, std::string("client"));
    REQUIRE_EQ(reverse.front().address, std::string("127.0.0.1"));
    // The same credential is located at a new source IP; no new pairing code.
    ClipboardSync::LanDiscovery moved("host");
    REQUIRE(moved.Open("127.0.0.3", 0, 3333));
    REQUIRE(client.Request(key, {{"127.0.0.3", moved.Port()}}));
    moved.Poll(Lookup("client"), stop);
    const auto relocated = client.Poll(Lookup("host"), stop);
    REQUIRE_EQ(relocated.size(), size_t{1});
    REQUIRE_EQ(relocated.front().device, std::string("host"));
    REQUIRE_EQ(relocated.front().key_id, key.key_id);
    REQUIRE_EQ(relocated.front().address, std::string("127.0.0.3"));
}

TEST_CASE(clipboard_discovery_accepts_only_fresh_solicited_reply_and_current_authorization)
{
    std::atomic<bool> stop{false};
    ClipboardSync::LanDiscovery client("client");
    REQUIRE(client.Open("127.0.0.1", 0, 1111));
    RawPeer host;
    REQUIRE(client.Request(key, {{"127.0.0.1", host.port}}));
    MsimeClipboardLan::DiscoveryMessage request;
    REQUIRE(ClipboardSync::VerifyDiscovery(host.Receive(), key.secret, request));
    auto response = request;
    response.type = MsimeClipboardLan::DiscoveryType::Reply;
    response.from = "host";
    response.to = "client";
    response.port = 2222;
    auto unsolicited = response;
    unsolicited.nonce = std::string(32, 'f');
    host.Reply(ClipboardSync::SignDiscovery(unsolicited, key.secret));
    REQUIRE(client.Poll(Lookup("host"), stop).empty());
    auto impersonated = response;
    impersonated.from = "other-host";
    host.Reply(ClipboardSync::SignDiscovery(impersonated, key.secret));
    REQUIRE(client.Poll(Lookup("host"), stop).empty());
    const auto valid = ClipboardSync::SignDiscovery(response, key.secret);
    host.Reply(valid);
    REQUIRE_EQ(client.Poll(Lookup("host"), stop).size(), size_t{1});
    host.Reply(valid);
    REQUIRE(client.Poll(Lookup("host"), stop).empty()); // replay
    REQUIRE(client.Request(key, {{"127.0.0.1", host.port}}));
    REQUIRE(ClipboardSync::VerifyDiscovery(host.Receive(), key.secret, request));
    response.nonce = request.nonce;
    host.Reply(ClipboardSync::SignDiscovery(response, key.secret));
    REQUIRE(client.Poll([](const auto &, const auto &) -> std::optional<std::string> { return std::nullopt; }, stop)
                .empty());
    stop = true;
    REQUIRE(client.Poll(Lookup("host"), stop).empty());
}
