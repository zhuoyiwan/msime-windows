#include "sync/clipboard_tls.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <openssl/rand.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
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

struct Socket
{
    SocketValue value = kInvalid;
    ~Socket()
    {
        if (value != kInvalid)
        {
#ifdef _WIN32
            closesocket(value);
#else
            close(value);
#endif
        }
    }
    bool Nonblocking()
    {
#ifdef _WIN32
        u_long enabled = 1;
        return ioctlsocket(value, FIONBIO, &enabled) == 0;
#else
        return fcntl(value, F_SETFL, O_NONBLOCK) == 0;
#endif
    }
};

bool Wait(SocketValue socket, bool write, Clock::time_point deadline, const std::atomic<bool> &stop)
{
    while (!stop && Clock::now() < deadline)
    {
        fd_set descriptors;
        FD_ZERO(&descriptors);
        FD_SET(socket, &descriptors);
        timeval interval{0, 50000};
        const int result = select(static_cast<int>(socket + 1), write ? nullptr : &descriptors,
                                  write ? &descriptors : nullptr, nullptr, &interval);
        if (result > 0)
            return !stop;
        if (result < 0)
            return false;
    }
    return false;
}

using Context = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;
using Connection = std::unique_ptr<SSL, decltype(&SSL_free)>;

struct SessionState
{
    const ClipboardSync::TlsCallbacks *callbacks = nullptr;
    std::string key_id;
    std::string secret;
};

// No exception may escape an OpenSSL C callback.
SSL_SESSION *PskSession(SSL *ssl, const std::string &secret)
{
    if (!MsimeClipboardLan::IsHex(secret, MsimeClipboardLan::kSecretBytes))
        return nullptr;
    unsigned char key[MsimeClipboardLan::kSecretBytes]{};
    auto nibble = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
    for (size_t i = 0; i < sizeof(key); ++i)
        key[i] = static_cast<unsigned char>((nibble(secret[i * 2]) << 4) | nibble(secret[i * 2 + 1]));
    const unsigned char cipher_id[] = {0x13, 0x01}; // TLS_AES_128_GCM_SHA256
    const auto *cipher = SSL_CIPHER_find(ssl, cipher_id);
    SSL_SESSION *session = SSL_SESSION_new();
    const bool ok = session && cipher && SSL_SESSION_set1_master_key(session, key, sizeof(key)) == 1 &&
                    SSL_SESSION_set_cipher(session, cipher) == 1 &&
                    SSL_SESSION_set_protocol_version(session, TLS1_3_VERSION) == 1;
    OPENSSL_cleanse(key, sizeof(key));
    if (!ok)
    {
        SSL_SESSION_free(session);
        return nullptr;
    }
    return session;
}

int ClientPsk(SSL *ssl, const EVP_MD *digest, const unsigned char **identity, size_t *length, SSL_SESSION **session)
{
    try
    {
        auto *state = static_cast<SessionState *>(SSL_get_app_data(ssl));
        if (!state || (digest && EVP_MD_get_type(digest) != NID_sha256))
            return 0;
        *identity = reinterpret_cast<const unsigned char *>(state->key_id.data());
        *length = state->key_id.size();
        *session = PskSession(ssl, state->secret);
        return *session ? 1 : 0;
    }
    catch (...)
    {
        return 0;
    }
}

int ServerPsk(SSL *ssl, const unsigned char *identity, size_t length, SSL_SESSION **session)
{
    *session = nullptr;
    try
    {
        auto *state = static_cast<SessionState *>(SSL_get_app_data(ssl));
        if (!state || length != MsimeClipboardLan::kKeyIdBytes * 2)
            return 0;
        state->key_id.assign(reinterpret_cast<const char *>(identity), length);
        if (!MsimeClipboardLan::IsHex(state->key_id, MsimeClipboardLan::kKeyIdBytes))
            return 0;
        const auto key = state->callbacks->lookup(state->key_id);
        if (!key)
            return 0;
        state->secret = *key;
        *session = PskSession(ssl, *key);
        return *session ? 1 : 0;
    }
    catch (...)
    {
        return 0;
    }
}

Context MakeContext(bool server)
{
    Context context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
    if (!context || SSL_CTX_set_min_proto_version(context.get(), TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_ciphersuites(context.get(), "TLS_AES_128_GCM_SHA256") != 1 ||
        SSL_CTX_set1_groups_list(context.get(), "X25519:P-256") != 1)
        return {nullptr, SSL_CTX_free};
    SSL_CTX_set_options(context.get(), SSL_OP_NO_TICKET);
    SSL_CTX_clear_options(context.get(), SSL_OP_ALLOW_NO_DHE_KEX);
    SSL_CTX_set_session_cache_mode(context.get(), SSL_SESS_CACHE_OFF);
    SSL_CTX_set_max_early_data(context.get(), 0);
    SSL_CTX_set_num_tickets(context.get(), 0);
    if (server)
        SSL_CTX_set_psk_find_session_callback(context.get(), ServerPsk);
    else
        SSL_CTX_set_psk_use_session_callback(context.get(), ClientPsk);
    return context;
}

struct Channel
{
    SSL *ssl;
    SocketValue socket;
    Clock::time_point deadline;
    const std::atomic<bool> &stop;

    bool Retry(int result)
    {
        const int error = SSL_get_error(ssl, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
            return false;
        return Wait(socket, error == SSL_ERROR_WANT_WRITE, deadline, stop);
    }

    bool Handshake(bool server)
    {
        while (!stop && Clock::now() < deadline)
        {
            ERR_clear_error();
            const int result = server ? SSL_accept(ssl) : SSL_connect(ssl);
            if (result == 1)
                return SSL_session_reused(ssl) == 1 && SSL_version(ssl) == TLS1_3_VERSION &&
                       SSL_get_negotiated_group(ssl) != 0;
            if (!Retry(result))
                return false;
        }
        return false;
    }

    bool Transfer(char *data, size_t size, bool write)
    {
        size_t offset = 0;
        while (offset < size && !stop && Clock::now() < deadline)
        {
            const int count = static_cast<int>((std::min)(size - offset, size_t{16384}));
            ERR_clear_error();
            const int result = write ? SSL_write(ssl, data + offset, count) : SSL_read(ssl, data + offset, count);
            if (result > 0)
                offset += static_cast<size_t>(result);
            else if (!Retry(result))
                return false;
        }
        return offset == size && !stop;
    }

    bool Send(std::string data)
    {
        return Transfer(data.data(), data.size(), true);
    }

    bool HelloSend(const std::string &device)
    {
        std::string hello(MsimeClipboardLan::kHello);
        hello.push_back(static_cast<char>(device.size()));
        hello += device;
        return MsimeClipboardSync::ValidIdentity(device) && Send(std::move(hello));
    }

    bool HelloReceive(std::string &device)
    {
        char header[7]{};
        if (!Transfer(header, sizeof(header), false) || std::string_view(header, 6) != MsimeClipboardLan::kHello)
            return false;
        const size_t length = static_cast<unsigned char>(header[6]);
        if (!length || length > MsimeClipboardSync::kMaxIdentityBytes)
            return false;
        device.resize(length);
        return Transfer(device.data(), device.size(), false) && MsimeClipboardSync::ValidIdentity(device);
    }

    bool SendBatch(const std::vector<MsimeClipboardSync::Record> &records, const std::string &local_device)
    {
        if (records.size() > MsimeClipboardSync::kMaxItems)
            return false;
        std::string batch(1, static_cast<char>(records.size()));
        for (const auto &record : records)
        {
            std::string frame;
            if (record.origin_device != local_device || record.created_at <= 0 ||
                !MsimeClipboardSync::Encode(record, frame))
                return false;
            batch += frame;
        }
        return Send(std::move(batch));
    }

    bool ReceiveBatch(const std::string &peer, const ClipboardSync::TlsCallbacks &callbacks)
    {
        char count{};
        if (!Transfer(&count, 1, false) || static_cast<unsigned char>(count) > MsimeClipboardSync::kMaxItems)
            return false;
        for (unsigned i = 0; i < static_cast<unsigned char>(count); ++i)
        {
            std::string frame(4, '\0');
            size_t length = 0;
            if (!Transfer(frame.data(), frame.size(), false) || !MsimeClipboardSync::FrameSize(frame, length))
                return false;
            frame.resize(length);
            MsimeClipboardSync::Record record;
            if (!Transfer(frame.data() + 4, length - 4, false) || !MsimeClipboardSync::Decode(frame, record) ||
                record.origin_device != peer || !callbacks.receive(record, peer))
                return false;
        }
        return true;
    }
};

bool ValidCallbacks(const ClipboardSync::TlsCallbacks &callbacks)
{
    return callbacks.lookup && callbacks.authorize && callbacks.outgoing && callbacks.receive && callbacks.acknowledged;
}
} // namespace

bool ClipboardSync::IsLanAddress(const std::string &address)
{
    if (!NetworkReady())
        return false;
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1)
        return false;
    const auto value = ntohl(parsed.s_addr);
    return (value >> 24) == 10 || (value >> 24) == 127 || (value >> 20) == 0xac1 || (value >> 16) == 0xc0a8 ||
           (value >> 16) == 0xa9fe;
}

std::string ClipboardSync::RandomHex(size_t bytes)
{
    if (!bytes || bytes > 128)
        return {};
    std::string random(bytes, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(random.data()), static_cast<int>(bytes)) != 1)
        return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char byte : random)
    {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    OPENSSL_cleanse(random.data(), random.size());
    return result;
}

struct ClipboardSync::TlsListener::Impl
{
    Socket socket;
    unsigned short port = 0;
};

ClipboardSync::TlsListener::TlsListener() : impl_(std::make_unique<Impl>())
{
}
ClipboardSync::TlsListener::~TlsListener() = default;

bool ClipboardSync::TlsListener::Open(const std::string &address, unsigned short port)
{
    if (!NetworkReady() || impl_->socket.value != kInvalid || (address != "0.0.0.0" && !IsLanAddress(address)))
        return false;
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1)
        return false;
    impl_->socket.value = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (impl_->socket.value == kInvalid)
        return false;
#ifdef _WIN32
    const int exclusive = 1;
    if (setsockopt(impl_->socket.value, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                   sizeof(exclusive)) != 0)
        return false;
#endif
    if (!impl_->socket.Nonblocking() ||
        bind(impl_->socket.value, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) != 0 ||
        listen(impl_->socket.value, 4) != 0)
        return false;
#ifdef _WIN32
    int length = sizeof(endpoint);
#else
    socklen_t length = sizeof(endpoint);
#endif
    if (getsockname(impl_->socket.value, reinterpret_cast<sockaddr *>(&endpoint), &length) != 0)
        return false;
    impl_->port = ntohs(endpoint.sin_port);
    return true;
}

unsigned short ClipboardSync::TlsListener::Port() const
{
    return impl_->port;
}

bool ClipboardSync::TlsListener::Poll(const std::string &local_device, const TlsCallbacks &callbacks,
                                      const std::atomic<bool> &stop)
{
    if (!impl_->port || !MsimeClipboardSync::ValidIdentity(local_device) || !ValidCallbacks(callbacks) ||
        !Wait(impl_->socket.value, false, Clock::now() + std::chrono::milliseconds(50), stop))
        return false;
    sockaddr_in remote{};
#ifdef _WIN32
    int length = sizeof(remote);
#else
    socklen_t length = sizeof(remote);
#endif
    Socket socket;
    socket.value = accept(impl_->socket.value, reinterpret_cast<sockaddr *>(&remote), &length);
    char address[INET_ADDRSTRLEN]{};
    if (socket.value == kInvalid || !inet_ntop(AF_INET, &remote.sin_addr, address, sizeof(address)) ||
        !IsLanAddress(address) || !socket.Nonblocking())
        return false;
    auto context = MakeContext(true);
    Connection ssl(context ? SSL_new(context.get()) : nullptr, SSL_free);
    SessionState state{&callbacks, {}, {}};
    if (!ssl)
        return false;
    SSL_set_app_data(ssl.get(), &state);
    if (SSL_set_fd(ssl.get(), static_cast<int>(socket.value)) != 1)
        return false;
    Channel channel{ssl.get(), socket.value, Clock::now() + std::chrono::seconds(3), stop};
    std::string peer;
    if (!channel.Handshake(true) || !channel.HelloReceive(peer) || peer == local_device ||
        !callbacks.authorize(state.key_id, peer) || !channel.HelloSend(local_device) ||
        !channel.ReceiveBatch(peer, callbacks) || !channel.SendBatch(callbacks.outgoing(peer), local_device))
        return false;
    char ack{};
    if (!channel.Transfer(&ack, 1, false) || ack != 1)
        return false;
    callbacks.acknowledged(peer);
    return true;
}

bool ClipboardSync::TlsExchange(const std::string &address, unsigned short port,
                                const MsimeClipboardLan::PairingCode &peer, const std::string &local_device,
                                const TlsCallbacks &callbacks, const std::atomic<bool> &stop)
{
    if (!IsLanAddress(address) || !port || MsimeClipboardLan::EncodePairingCode(peer).empty() ||
        !MsimeClipboardSync::ValidIdentity(local_device) || peer.device == local_device || !ValidCallbacks(callbacks))
        return false;
    Socket socket;
    socket.value = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket.value == kInvalid || !socket.Nonblocking())
        return false;
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &endpoint.sin_addr) != 1)
        return false;
    const auto deadline = Clock::now() + std::chrono::seconds(3);
    // Nonblocking connect: writability alone is insufficient; check SO_ERROR too.
    connect(socket.value, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint));
    if (!Wait(socket.value, true, deadline, stop))
        return false;
    int error = 0;
#ifdef _WIN32
    int length = sizeof(error);
#else
    socklen_t length = sizeof(error);
#endif
    if (getsockopt(socket.value, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length) != 0 || error)
        return false;
    auto context = MakeContext(false);
    Connection ssl(context ? SSL_new(context.get()) : nullptr, SSL_free);
    SessionState state{&callbacks, peer.key_id, peer.secret};
    if (!ssl)
        return false;
    SSL_set_app_data(ssl.get(), &state);
    if (SSL_set_fd(ssl.get(), static_cast<int>(socket.value)) != 1)
        return false;
    Channel channel{ssl.get(), socket.value, deadline, stop};
    std::string device;
    if (!channel.Handshake(false) || !channel.HelloSend(local_device) || !channel.HelloReceive(device) ||
        device != peer.device || !callbacks.authorize(peer.key_id, device) ||
        !channel.SendBatch(callbacks.outgoing(device), local_device) || !channel.ReceiveBatch(device, callbacks) ||
        !channel.Send(std::string(1, '\x01')))
        return false;
    callbacks.acknowledged(device);
    return true;
}
