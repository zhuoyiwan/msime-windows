#include "sync/clipboard_sync_service.h"
#include "sync/clipboard_tls.h"
#include "clipboard/clipboard_history.h"
#include "utils/common_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace
{
struct Service
{
    std::atomic<bool> stop{true};
    std::mutex mutex;
    std::condition_variable wake;
    std::thread listener, sender;
    HANDLE reset_event = nullptr;
    std::vector<ClipboardSync::PairedDevice> peers;
    std::map<std::string, std::vector<MsimeClipboardSync::Record>> pending;
};
Service g_service;
std::mutex g_lifecycle;

// Both workers and the capture callback serialize the reset with queue access. A brief disable /
// enable in the separate Settings process must clear queues even when polling misses that interval.
void ApplyResetLocked()
{
    if (g_service.reset_event && WaitForSingleObject(g_service.reset_event, 0) == WAIT_OBJECT_0)
    {
        g_service.pending.clear();
        ResetEvent(g_service.reset_event);
    }
}

void RefreshPeers(const ClipboardSync::PairingStore &store)
{
    const auto devices = store.Devices();
    std::lock_guard<std::mutex> lock(g_service.mutex);
    ApplyResetLocked();
    g_service.peers = devices.value_or(std::vector<ClipboardSync::PairedDevice>{});
    std::set<std::string> active;
    for (const auto &peer : g_service.peers)
        active.insert(peer.key.key_id);
    for (auto it = g_service.pending.begin(); it != g_service.pending.end();)
        if (!active.count(it->first) || !ClipboardHistory::IsSyncEnabled())
            it = g_service.pending.erase(it);
        else
            ++it;
}

void Capture(const MsimeClipboardSync::Record &record)
{
    if (!ClipboardHistory::IsSyncEnabled() || g_service.stop)
        return;
    std::lock_guard<std::mutex> lock(g_service.mutex);
    ApplyResetLocked();
    for (const auto &peer : g_service.peers)
    {
        auto &queue = g_service.pending[peer.key.key_id];
        queue.push_back(record);
        if (queue.size() > MsimeClipboardSync::kMaxItems)
            queue.erase(queue.begin());
    }
    g_service.wake.notify_all();
}

ClipboardSync::TlsCallbacks Callbacks(const ClipboardSync::PairingStore &store)
{
    auto sent = std::make_shared<std::map<std::string, std::set<std::string>>>();
    auto authenticated = std::make_shared<std::map<std::string, std::string>>();
    return {[&store](const std::string &key) -> std::optional<std::string> {
                return ClipboardHistory::IsSyncEnabled() ? store.Lookup(key, ClipboardSync::UnixNow()) : std::nullopt;
            },
            [&store, authenticated](const std::string &key, const std::string &device) {
                if (!ClipboardHistory::IsSyncEnabled() || !store.Authorize(key, device, ClipboardSync::UnixNow()))
                    return false;
                (*authenticated)[device] = key;
                RefreshPeers(store);
                return true;
            },
            [&store, sent, authenticated](const std::string &device) {
                if (!ClipboardHistory::IsSyncEnabled() || !authenticated->count(device) ||
                    !store.Authorize(authenticated->at(device), device, ClipboardSync::UnixNow()))
                    return std::vector<MsimeClipboardSync::Record>{};
                const auto key = authenticated->at(device);
                std::lock_guard<std::mutex> lock(g_service.mutex);
                ApplyResetLocked();
                auto records = g_service.pending[key];
                for (const auto &record : records)
                    (*sent)[key].insert(record.id);
                return records;
            },
            [&store, authenticated](const MsimeClipboardSync::Record &record, const std::string &device) {
                if (!ClipboardHistory::IsSyncEnabled() || !authenticated->count(device) ||
                    !store.Authorize(authenticated->at(device), device, ClipboardSync::UnixNow()))
                    return false;
                const auto result = ClipboardHistory::ImportRecord(record, device);
                return result == ClipboardSync::ImportResult::Imported ||
                       result == ClipboardSync::ImportResult::Duplicate;
            },
            [sent](const std::string &) {
                std::lock_guard<std::mutex> lock(g_service.mutex);
                for (const auto &[key, ids] : *sent)
                {
                    auto &queue = g_service.pending[key];
                    queue.erase(std::remove_if(queue.begin(), queue.end(),
                                               [&](const auto &record) { return ids.count(record.id) != 0; }),
                                queue.end());
                }
            }};
}

void Listen(const std::string &local_device, const ClipboardSync::PairingStore &store, unsigned short port,
            const std::string &bind_address)
{
    std::unique_ptr<ClipboardSync::TlsListener> listener;
    bool previously_enabled = false;
    while (!g_service.stop)
    {
        const bool enabled = ClipboardHistory::IsSyncEnabled();
        if (!enabled)
        {
            listener.reset();
            if (previously_enabled)
                store.CancelInvite();
        }
        else if (!listener)
        {
            auto candidate = std::make_unique<ClipboardSync::TlsListener>();
            if (candidate->Open(bind_address, port))
                listener = std::move(candidate);
        }
        previously_enabled = enabled;
        if (listener)
            listener->Poll(local_device, Callbacks(store), g_service.stop);
        else
        {
            std::unique_lock<std::mutex> lock(g_service.mutex);
            g_service.wake.wait_for(lock, std::chrono::milliseconds(200), [] { return g_service.stop.load(); });
        }
    }
}

void Send(const std::string &local_device, const ClipboardSync::PairingStore &store, unsigned short port)
{
    while (!g_service.stop)
    {
        RefreshPeers(store);
        std::vector<ClipboardSync::PairedDevice> peers;
        {
            std::lock_guard<std::mutex> lock(g_service.mutex);
            peers = g_service.peers;
        }
        for (const auto &peer : peers)
        {
            if (g_service.stop || !ClipboardHistory::IsSyncEnabled())
                break;
            if (!peer.address.empty())
                ClipboardSync::TlsExchange(peer.address, port, peer.key, local_device, Callbacks(store),
                                           g_service.stop);
        }
        std::unique_lock<std::mutex> lock(g_service.mutex);
        g_service.wake.wait_for(lock, std::chrono::seconds(1), [] { return g_service.stop.load(); });
    }
}
} // namespace

int64_t ClipboardSync::UnixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ClipboardSync::PairingStore ClipboardSync::OpenPairingStore()
{
    const auto root = CommonUtils::get_ime_data_path_w();
    return PairingStore(root.empty() ? std::filesystem::path{} : std::filesystem::path(root) / L"clipboard_peers.dat",
                        ClipboardHistory::LocalDeviceId());
}

void ClipboardSync::StartLanService(unsigned short port, const std::string &bind_address)
{
    std::lock_guard<std::mutex> lifecycle(g_lifecycle);
    if (!g_service.stop)
        return;
    const auto local_device = ClipboardHistory::LocalDeviceId();
    if (local_device.empty())
        return;
    auto store = OpenPairingStore();
    g_service.reset_event = CreateEventW(nullptr, TRUE, FALSE, ClipboardHistory::kSyncResetEvent);
    if (!g_service.reset_event)
        return;
    RefreshPeers(store);
    g_service.stop = false;
    ClipboardHistory::SetCaptureObserver(Capture);
    g_service.listener = std::thread(
        [local_device, store, port, bind_address]() mutable { Listen(local_device, store, port, bind_address); });
    g_service.sender = std::thread([local_device, store, port]() mutable { Send(local_device, store, port); });
}

void ClipboardSync::StopLanService()
{
    std::lock_guard<std::mutex> lifecycle(g_lifecycle);
    ClipboardHistory::SetCaptureObserver({});
    g_service.stop = true;
    g_service.wake.notify_all();
    if (g_service.listener.joinable())
        g_service.listener.join();
    if (g_service.sender.joinable())
        g_service.sender.join();
    std::lock_guard<std::mutex> lock(g_service.mutex);
    g_service.peers.clear();
    g_service.pending.clear();
    if (g_service.reset_event)
    {
        CloseHandle(g_service.reset_event);
        g_service.reset_event = nullptr;
    }
}

std::string ClipboardSync::SettingsRequest(const std::string &action, const std::string &code,
                                           const std::string &address, const std::string &device)
{
    using Json = nlohmann::json;
    auto store = OpenPairingStore();
    bool ok = false;
    std::string invitation;
    if (action == "status")
        ok = true;
    else if (action == "cancel")
        ok = store.CancelInvite();
    else if (action == "remove")
        ok = store.RemovePeer(device);
    else if (ClipboardHistory::IsSyncEnabled())
    {
        if (action == "invite")
        {
            invitation = store.CreateInvite(UnixNow());
            ok = !invitation.empty();
        }
        else if (action == "pair")
            ok = store.AddPeer(code, address);
    }
    const auto devices = store.Devices();
    Json response{{"ok", ok && devices.has_value()},
                  {"message", ok && devices ? "已更新；连接方会自动重试，设备 IP 改变后需重新配对"
                                            : "操作失败：请检查两个开关、私网 IP、配对码及设备是否已存在"},
                  {"localDevice", ClipboardHistory::LocalDeviceId()},
                  {"devices", Json::array()}};
    if (!invitation.empty())
    {
        response["code"] = invitation;
        response["expiresIn"] = MsimeClipboardLan::kInviteSeconds;
    }
    if (devices)
        for (const auto &peer : *devices)
            response["devices"].push_back({{"device", peer.key.device}, {"address", peer.address}});
    return response.dump();
}
