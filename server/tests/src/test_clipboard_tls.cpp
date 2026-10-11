#include "sync/clipboard_tls.h"
#include "clipboard/clipboard_record.h"
#include "tests/includes/test_framework.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <mutex>
#include <thread>
#ifndef _WIN32
#include <csignal>
#endif

namespace
{
using MsimeClipboardSync::Record;

Record Capture(std::string id, std::string device, std::string text)
{
    const auto hash = ClipboardRecord::HashText(text);
    return {std::move(id), std::move(text), hash, std::move(device), 42};
}

class Loopback
{
  public:
    MsimeClipboardLan::PairingCode key{"host", ClipboardSync::RandomHex(16), ClipboardSync::RandomHex(32)};
    std::atomic<bool> stop{false};
    std::atomic<unsigned> imported{0}, acknowledged{0};
    ClipboardSync::TlsListener listener;
    std::thread thread;
    std::string allowed = "client";
    std::vector<Record> outbound{Capture("server-record", "host", "主机中文 😀")};

    Loopback()
    {
#ifndef _WIN32
        // OpenSSL's POSIX socket BIO reports EPIPE; the test process must not terminate on SIGPIPE.
        std::signal(SIGPIPE, SIG_IGN);
#endif
        REQUIRE(listener.Open("127.0.0.1", 0));
    }
    ~Loopback()
    {
        stop = true;
        if (thread.joinable())
            thread.join();
    }
    void Start()
    {
        thread = std::thread([this] {
            auto callbacks = ClientCallbacks();
            callbacks.outgoing = [this](const std::string &) { return outbound; };
            callbacks.authorize = [this](const std::string &id, const std::string &device) {
                return id == key.key_id && device == allowed;
            };
            while (!stop)
                listener.Poll("host", callbacks, stop);
        });
    }
    ClipboardSync::TlsCallbacks ClientCallbacks()
    {
        return {
            [this](const std::string &id) -> std::optional<std::string> {
                return id == key.key_id ? std::optional<std::string>(key.secret) : std::nullopt;
            },
            [this](const std::string &id, const std::string &device) { return id == key.key_id && device == "host"; },
            [](const std::string &) { return std::vector<Record>{Capture("client-record", "client", "客户端 😀")}; },
            [this](const Record &record, const std::string &peer) {
                if (record.origin_device != peer || record.hash != ClipboardRecord::HashText(record.text))
                    return false;
                ++imported;
                return true;
            },
            [this](const std::string &) { ++acknowledged; }};
    }
    bool Exchange(const MsimeClipboardLan::PairingCode &candidate, const std::string &device = "client")
    {
        return ClipboardSync::TlsExchange("127.0.0.1", listener.Port(), candidate, device, ClientCallbacks(), stop);
    }
};

struct PlainSocket
{
#ifdef _WIN32
    SOCKET socket = INVALID_SOCKET;
    ~PlainSocket()
    {
        if (socket != INVALID_SOCKET)
            closesocket(socket);
    }
#else
    int socket = -1;
    ~PlainSocket()
    {
        if (socket >= 0)
            close(socket);
    }
#endif
    bool Connect(unsigned short port)
    {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &endpoint.sin_addr);
        return connect(socket, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == 0;
    }
};
} // namespace

TEST_CASE(clipboard_lan_pairing_code_strict_validation_and_private_addresses)
{
    const MsimeClipboardLan::PairingCode original{"device.id", std::string(32, 'a'), std::string(64, 'b')};
    const auto code = MsimeClipboardLan::EncodePairingCode(original);
    MsimeClipboardLan::PairingCode parsed;
    REQUIRE(MsimeClipboardLan::DecodePairingCode(code, parsed));
    REQUIRE_EQ(parsed.device, original.device);
    REQUIRE_EQ(parsed.secret, original.secret);
    for (const auto &bad : {code + ":extra", code.substr(1), code.substr(0, code.size() - 1),
                            std::string("msime1:bad/id:") + original.key_id + ":" + original.secret})
    {
        REQUIRE(!MsimeClipboardLan::DecodePairingCode(bad, parsed));
        REQUIRE(parsed.secret.empty());
    }
    for (const auto &address : {"10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.0.1", "169.254.2.3", "127.0.0.1"})
        REQUIRE(ClipboardSync::IsLanAddress(address));
    for (const auto &address : {"8.8.8.8", "172.15.0.1", "172.32.0.1", "192.169.0.1", "0.0.0.0", "224.0.0.1",
                                "example.com", "127.1", "192.168.1.1:49876", "::1"})
        REQUIRE(!ClipboardSync::IsLanAddress(address));
}

TEST_CASE(clipboard_tls_real_duplex_exchange_with_utf8_records_and_ack)
{
    Loopback pair;
    pair.Start();
    REQUIRE(pair.Exchange(pair.key));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (pair.acknowledged != 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_EQ(pair.imported.load(), 2u);
    REQUIRE_EQ(pair.acknowledged.load(), 2u);
    // Each reconnect uses a new full TLS handshake and fresh traffic keys, with the same long-term PSK.
    REQUIRE(pair.Exchange(pair.key));
    REQUIRE_EQ(pair.imported.load(), 4u);
}

TEST_CASE(clipboard_tls_wrong_secret_unknown_key_and_identity_cannot_import)
{
    Loopback pair;
    pair.Start();
    auto bad = pair.key;
    bad.secret = ClipboardSync::RandomHex(32);
    REQUIRE(!pair.Exchange(bad));
    bad = pair.key;
    bad.key_id = ClipboardSync::RandomHex(16);
    REQUIRE(!pair.Exchange(bad));
    REQUIRE(!pair.Exchange(pair.key, "impersonator"));
    REQUIRE(!pair.Exchange(pair.key, "host"));
    REQUIRE_EQ(pair.imported.load(), 0u);
    REQUIRE_EQ(pair.acknowledged.load(), 0u);
    REQUIRE(pair.Exchange(pair.key));
}

TEST_CASE(clipboard_tls_expected_host_binding_and_stop_fail_closed)
{
    Loopback pair;
    pair.Start();
    auto wrong_host = pair.key;
    wrong_host.device = "different-host";
    REQUIRE(!pair.Exchange(wrong_host));
    REQUIRE_EQ(pair.imported.load(), 0u);
    pair.stop = true;
    const auto begin = std::chrono::steady_clock::now();
    REQUIRE(!pair.Exchange(pair.key));
    REQUIRE(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(200));
}

TEST_CASE(clipboard_tls_rejects_unpublishable_and_oversized_batches)
{
    Loopback pair;
    pair.outbound[0].origin_device = "third-device";
    pair.Start();
    REQUIRE(!pair.Exchange(pair.key));
    REQUIRE_EQ(pair.acknowledged.load(), 0u);
    REQUIRE_EQ(pair.imported.load(), 1u); // client event may import, but failed exchange is never acknowledged
    Loopback oversized;
    oversized.outbound.resize(51, Capture("id", "host", "text"));
    oversized.Start();
    REQUIRE(!oversized.Exchange(oversized.key));
    REQUIRE_EQ(oversized.acknowledged.load(), 0u);
}

TEST_CASE(clipboard_tls_receiver_rejects_digest_mismatch)
{
    Loopback pair;
    pair.outbound[0].hash = std::string(64, '0');
    pair.Start();
    REQUIRE(!pair.Exchange(pair.key));
    REQUIRE_EQ(pair.acknowledged.load(), 0u);
}

TEST_CASE(clipboard_tls_plaintext_frame_cannot_bypass_authentication)
{
    Loopback pair;
    pair.Start();
    PlainSocket raw;
    REQUIRE(raw.Connect(pair.listener.Port()));
    std::string frame;
    REQUIRE(MsimeClipboardSync::Encode(Capture("untrusted", "client", "not encrypted"), frame));
    REQUIRE(send(raw.socket, frame.data(), static_cast<int>(frame.size()), 0) > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE_EQ(pair.imported.load(), 0u);
    REQUIRE_EQ(pair.acknowledged.load(), 0u);
}

TEST_CASE(clipboard_tls_stop_interrupts_an_incomplete_handshake)
{
    Loopback pair;
    pair.Start();
    PlainSocket raw;
    REQUIRE(raw.Connect(pair.listener.Port()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto begin = std::chrono::steady_clock::now();
    pair.stop = true;
    pair.thread.join();
    REQUIRE(std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(500));
    REQUIRE_EQ(pair.imported.load(), 0u);
}
