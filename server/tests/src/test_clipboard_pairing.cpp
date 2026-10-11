#include "sync/clipboard_pairing.h"
#include "sync/clipboard_sync_service.h"
#include "sync/clipboard_tls.h"
#include "clipboard/clipboard_history.h"
#include "config/credential_protection.h"
#include "tests/includes/test_framework.h"

#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
struct Fixture
{
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("msime-pairing-" + ClipboardSync::RandomHex(16));
    std::wstring previous;
    Fixture()
    {
        const DWORD size = GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", nullptr, 0);
        if (size)
        {
            previous.resize(size);
            GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", previous.data(), size);
            previous.resize(size - 1);
        }
        std::filesystem::create_directories(directory);
        SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", directory.c_str());
        Enable(true);
    }
    ~Fixture()
    {
        ClipboardSync::StopLanService();
        ClipboardHistory::SetCaptureObserver({});
        SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", previous.empty() ? nullptr : previous.c_str());
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
    void Enable(bool enabled)
    {
        std::ofstream(directory / "config.toml")
            << "[utility]\nclipboard_history = true\nclipboard_lan_sync = " << (enabled ? "true" : "false") << '\n';
    }
    std::string Read(const std::filesystem::path &path)
    {
        std::ifstream input(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    }
};
} // namespace

TEST_CASE(clipboard_pairing_single_use_invite_dpapi_persistence_and_revoke)
{
    Fixture fixture;
    ClipboardSync::PairingStore host(fixture.directory / "host.dat", "host");
    const auto code = host.CreateInvite(1000);
    MsimeClipboardLan::PairingCode key;
    REQUIRE(MsimeClipboardLan::DecodePairingCode(code, key));
    const auto ciphertext = fixture.Read(fixture.directory / "host.dat");
    REQUIRE(CredentialProtection::IsProtected(ciphertext));
    REQUIRE(ciphertext.find(key.secret) == std::string::npos);
    REQUIRE(host.Lookup(key.key_id, 1299).has_value());
    REQUIRE(!host.Lookup(key.key_id, 1300));
    REQUIRE(!host.Authorize(key.key_id, "peer", 1300));
    REQUIRE(host.Authorize(key.key_id, "peer", 1299));
    REQUIRE(!host.Authorize(key.key_id, "another-peer", 1299));
    REQUIRE(host.Authorize(key.key_id, "peer", 5000));
    ClipboardSync::PairingStore reopened(fixture.directory / "host.dat", "host");
    const auto peers = reopened.Devices();
    REQUIRE(peers && peers->size() == 1);
    REQUIRE_EQ(peers->front().key.device, std::string("peer"));
    REQUIRE(reopened.RemovePeer("peer"));
    REQUIRE(!host.Lookup(key.key_id, 1299));
    REQUIRE(!host.Authorize(key.key_id, "peer", 1299));
}

TEST_CASE(clipboard_pairing_cancel_replacement_self_and_corrupt_store_fail_closed)
{
    Fixture fixture;
    ClipboardSync::PairingStore store(fixture.directory / "peers.dat", "host");
    MsimeClipboardLan::PairingCode first, second;
    REQUIRE(MsimeClipboardLan::DecodePairingCode(store.CreateInvite(1000), first));
    REQUIRE(MsimeClipboardLan::DecodePairingCode(store.CreateInvite(1000), second));
    REQUIRE(!store.Lookup(first.key_id, 1001));
    REQUIRE(!store.Authorize(second.key_id, "host", 1001));
    REQUIRE(store.CancelInvite());
    REQUIRE(!store.Lookup(second.key_id, 1001));
    REQUIRE(!store.AddPeer(MsimeClipboardLan::EncodePairingCode(first), "127.0.0.1"));
    ClipboardSync::PairingStore wrong_identity(fixture.directory / "peers.dat", "different-device");
    REQUIRE(!wrong_identity.Devices());
    std::ofstream(fixture.directory / "peers.dat", std::ios::trunc) << "corrupt";
    REQUIRE(!store.Devices());
    REQUIRE(store.CreateInvite(1000).empty());
    REQUIRE(!store.CancelInvite());
    REQUIRE_EQ(fixture.Read(fixture.directory / "peers.dat"), std::string("corrupt"));
}

TEST_CASE(clipboard_pairing_peer_limit_duplicate_and_public_address_rejected)
{
    Fixture fixture;
    ClipboardSync::PairingStore store(fixture.directory / "peers.dat", "host");
    for (size_t i = 0; i < MsimeClipboardLan::kMaxPeers; ++i)
    {
        const auto code = MsimeClipboardLan::EncodePairingCode(
            {"peer-" + std::to_string(i), ClipboardSync::RandomHex(16), ClipboardSync::RandomHex(32)});
        REQUIRE(!store.AddPeer(code, "8.8.8.8"));
        REQUIRE(store.AddPeer(code, "127.0.0.1"));
        REQUIRE(!store.AddPeer(code, "127.0.0.1"));
    }
    REQUIRE(store.CreateInvite(1000).empty());
    REQUIRE(!store.AddPeer(
        MsimeClipboardLan::EncodePairingCode({"overflow", ClipboardSync::RandomHex(16), ClipboardSync::RandomHex(32)}),
        "127.0.0.1"));
}

TEST_CASE(clipboard_capture_observer_only_sees_successful_local_records)
{
    Fixture fixture;
    const auto local = ClipboardHistory::LocalDeviceId();
    REQUIRE(!local.empty());
    REQUIRE_EQ(ClipboardHistory::LocalDeviceId(), local);
    unsigned captures = 0;
    ClipboardHistory::SetCaptureObserver([&](const auto &record) {
        ++captures;
        REQUIRE_EQ(record.origin_device, local);
        REQUIRE(!ClipboardHistory::LoadRecords().empty()); // callback runs after releasing the history mutex
    });
    REQUIRE(ClipboardHistory::AddText(L"本机复制"));
    REQUIRE(!ClipboardHistory::AddText(L"本机复制"));
    REQUIRE(!ClipboardHistory::AddText(L"msime1:host:secret-pairing-code"));
    const MsimeClipboardSync::Record record{"remote-record", "远程内容", ClipboardRecord::HashText("远程内容"), "peer",
                                            42};
    REQUIRE(ClipboardHistory::ImportRecord(record, "peer") == ClipboardSync::ImportResult::Imported);
    ClipboardHistory::LoadRecords();
    ClipboardHistory::Clear();
    REQUIRE_EQ(captures, 1u);
    REQUIRE_EQ(ClipboardHistory::LocalDeviceId(), local);
}

TEST_CASE(clipboard_tls_pairs_then_imports_into_real_history_without_rebroadcast)
{
    Fixture fixture;
    const auto local = ClipboardHistory::LocalDeviceId();
    auto store = ClipboardSync::OpenPairingStore();
    MsimeClipboardLan::PairingCode invitation;
    REQUIRE(MsimeClipboardLan::DecodePairingCode(store.CreateInvite(ClipboardSync::UnixNow()), invitation));
    ClipboardSync::TlsListener listener;
    REQUIRE(listener.Open("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    unsigned captures = 0;
    ClipboardHistory::SetCaptureObserver([&](const auto &) { ++captures; });
    ClipboardSync::TlsCallbacks callbacks{
        [&](const auto &key) { return store.Lookup(key, ClipboardSync::UnixNow()); },
        [&](const auto &key, const auto &device) { return store.Authorize(key, device, ClipboardSync::UnixNow()); },
        [](const auto &) { return std::vector<MsimeClipboardSync::Record>{}; },
        [&](const auto &record, const auto &peer) {
            const auto result = ClipboardHistory::ImportRecord(record, peer);
            return result == ClipboardSync::ImportResult::Imported || result == ClipboardSync::ImportResult::Duplicate;
        },
        [](const auto &) {}};
    std::thread host([&] {
        while (!stop)
            listener.Poll(local, callbacks, stop);
    });
    auto client = callbacks;
    client.authorize = [&](const auto &key, const auto &device) { return key == invitation.key_id && device == local; };
    client.outgoing = [](const auto &) {
        return std::vector<MsimeClipboardSync::Record>{
            {"id", "跨设备文本 😀", ClipboardRecord::HashText("跨设备文本 😀"), "peer", 42}};
    };
    const bool success = ClipboardSync::TlsExchange("127.0.0.1", listener.Port(), invitation, "peer", client, stop);
    stop = true;
    host.join();
    REQUIRE(success);
    const auto records = ClipboardHistory::LoadRecords();
    REQUIRE_EQ(records.size(), size_t{1});
    REQUIRE_EQ(records.front().origin_device, std::string("peer"));
    REQUIRE_EQ(captures, 0u);
    REQUIRE(store.RemovePeer("peer"));
    REQUIRE(!store.Lookup(invitation.key_id, ClipboardSync::UnixNow()));
}

TEST_CASE(clipboard_settings_opt_in_and_disabled_service_shutdown)
{
    Fixture fixture;
    fixture.Enable(false);
    REQUIRE(!ClipboardHistory::IsSyncEnabled());
    const auto response = ClipboardSync::SettingsRequest("invite");
    REQUIRE(response.find("\"ok\":false") != std::string::npos);
    ClipboardSync::StartLanService();
    const auto begin = std::chrono::steady_clock::now();
    ClipboardSync::StopLanService();
    REQUIRE(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1));
}

TEST_CASE(clipboard_service_publishes_fresh_capture_and_imports_remote_history)
{
    Fixture fixture;
    const auto local = ClipboardHistory::LocalDeviceId();
    const MsimeClipboardLan::PairingCode key{"remote", ClipboardSync::RandomHex(16), ClipboardSync::RandomHex(32)};
    REQUIRE(ClipboardSync::OpenPairingStore().AddPeer(MsimeClipboardLan::EncodePairingCode(key), "127.0.0.1"));
    REQUIRE(ClipboardHistory::AddText(L"启动前历史不应发送"));
    ClipboardSync::TlsListener remote;
    REQUIRE(remote.Open("127.0.0.1", 0));
    std::atomic<bool> stop{false};
    std::atomic<unsigned> received{0}, unexpected{0};
    ClipboardSync::TlsCallbacks callbacks{
        [&](const auto &id) -> std::optional<std::string> {
            return id == key.key_id ? std::optional<std::string>(key.secret) : std::nullopt;
        },
        [&](const auto &id, const auto &device) { return id == key.key_id && device == local; },
        [](const auto &) {
            return std::vector<MsimeClipboardSync::Record>{
                {"remote-id", "远端服务记录 😀", ClipboardRecord::HashText("远端服务记录 😀"), "remote", 42}};
        },
        [&](const auto &record, const auto &device) {
            if (device != local || record.text != "本机服务记录 😀")
                ++unexpected;
            else
                ++received;
            return true;
        },
        [](const auto &) {}};
    std::thread host([&] {
        while (!stop)
            remote.Poll("remote", callbacks, stop);
    });
    // No assertion may unwind past a joinable thread. Stop and join before assessing results.
    ClipboardSync::StartLanService(remote.Port(), "127.0.0.2");
    const bool captured = ClipboardHistory::AddText(L"本机服务记录 😀");
    bool imported = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        for (const auto &record : ClipboardHistory::LoadRecords())
            imported = imported || record.id == "remote-id";
        if (received && imported)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ClipboardSync::StopLanService();
    stop = true;
    host.join();
    REQUIRE(captured);
    REQUIRE(imported);
    REQUIRE(received.load() > 0);
    REQUIRE_EQ(unexpected.load(), 0u); // old history and imports never become outbound captures
}
