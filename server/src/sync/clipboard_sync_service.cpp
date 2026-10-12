#include "sync/clipboard_sync_service.h"
#include "sync/clipboard_tls.h"
#include "sync/clipboard_discovery.h"
#include "clipboard/clipboard_history.h"
#include "utils/common_utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
    std::thread listener, sender, discovery;
    HANDLE reset_event = nullptr;
    std::vector<ClipboardSync::PairedDevice> peers;
    std::map<std::string, std::vector<MsimeClipboardSync::Record>> pending;
    std::deque<MsimeClipboardSync::Record> received;
    std::map<std::string, std::pair<ClipboardSync::DiscoveredPeer, std::chrono::steady_clock::time_point>> endpoints;
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
        g_service.endpoints.clear();
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
    for (auto it = g_service.endpoints.begin(); it != g_service.endpoints.end();)
        if (!active.count(it->first) || !ClipboardHistory::IsSyncEnabled() ||
            std::chrono::steady_clock::now() - it->second.second > std::chrono::seconds(30))
            it = g_service.endpoints.erase(it);
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
                ClipboardSync::ImportResult result;
                {
                    // 历史按文字合并后，旧 ID 也必须记住，避免丢失确认后的重传覆盖新复制。
                    std::lock_guard<std::mutex> lock(g_service.mutex);
                    for (const auto &seen : g_service.received)
                        if (seen.id == record.id)
                            return seen.origin_device == record.origin_device && seen.text == record.text &&
                                   seen.hash == record.hash && seen.created_at == record.created_at;
                    result = ClipboardHistory::ImportRecord(record, device);
                    if (result != ClipboardSync::ImportResult::Imported &&
                        result != ClipboardSync::ImportResult::Duplicate)
                        return false;
                    g_service.received.push_back(record);
                    if (g_service.received.size() > MsimeClipboardSync::kMaxItems * 8)
                        g_service.received.pop_front();
                }
                if (result == ClipboardSync::ImportResult::Imported)
                    ClipboardMonitor::ApplyRemoteText(string_to_wstring(record.text));
                return true;
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
            ClipboardSync::DiscoveredPeer endpoint{peer.key.device, peer.key.key_id, peer.address, port};
            {
                std::lock_guard<std::mutex> lock(g_service.mutex);
                const auto found = g_service.endpoints.find(peer.key.key_id);
                if (found != g_service.endpoints.end() && found->second.first.device == peer.key.device)
                    endpoint = found->second.first;
            }
            if (!endpoint.address.empty() && ClipboardSync::TlsExchange(endpoint.address, endpoint.port, peer.key,
                                                                        local_device, Callbacks(store), g_service.stop))
                store.RememberAddress(peer.key.key_id, peer.key.device, endpoint.address);
        }
        std::unique_lock<std::mutex> lock(g_service.mutex);
        g_service.wake.wait_for(lock, std::chrono::seconds(1), [] { return g_service.stop.load(); });
    }
}

void Discover(const std::string &local_device, const ClipboardSync::PairingStore &store, unsigned short tcp_port,
              unsigned short udp_port, const std::string &bind_address)
{
    std::unique_ptr<ClipboardSync::LanDiscovery> discovery;
    auto next_request = std::chrono::steady_clock::now();
    while (!g_service.stop)
    {
        if (!ClipboardHistory::IsSyncEnabled())
        {
            discovery.reset();
            std::lock_guard<std::mutex> lock(g_service.mutex);
            g_service.endpoints.clear();
        }
        else if (!discovery)
        {
            auto candidate = std::make_unique<ClipboardSync::LanDiscovery>(local_device);
            if (candidate->Open(bind_address, udp_port, tcp_port))
            {
                discovery = std::move(candidate);
                next_request = std::chrono::steady_clock::now();
            }
        }
        if (!discovery)
        {
            std::unique_lock<std::mutex> lock(g_service.mutex);
            g_service.wake.wait_for(lock, std::chrono::milliseconds(200), [] { return g_service.stop.load(); });
            continue;
        }
        if (std::chrono::steady_clock::now() >= next_request)
        {
            const auto peers = store.Devices();
            const auto broadcasts = bind_address == "0.0.0.0" ? ClipboardSync::LanBroadcasts(udp_port)
                                                              : std::vector<ClipboardSync::DiscoveryDestination>{};
            if (peers)
                for (const auto &peer : *peers)
                {
                    auto targets = broadcasts;
                    if (!peer.address.empty())
                        targets.push_back({peer.address, udp_port});
                    discovery->Request(peer.key, targets);
                }
            next_request = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }
        const auto endpoints = discovery->Poll(
            [&store](const std::string &key, const std::string &device) {
                return ClipboardHistory::IsSyncEnabled() ? store.DiscoverySecret(key, device, ClipboardSync::UnixNow())
                                                         : std::nullopt;
            },
            g_service.stop);
        if (!endpoints.empty() && ClipboardHistory::IsSyncEnabled())
        {
            std::lock_guard<std::mutex> lock(g_service.mutex);
            for (const auto &endpoint : endpoints)
                g_service.endpoints[endpoint.key_id] = {endpoint, std::chrono::steady_clock::now()};
            g_service.wake.notify_all();
        }
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

void ClipboardSync::StartLanService(unsigned short port, const std::string &bind_address, unsigned short discovery_port)
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
    g_service.discovery = std::thread([local_device, store, port, bind_address, discovery_port]() mutable {
        Discover(local_device, store, port, discovery_port, bind_address);
    });
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
    if (g_service.discovery.joinable())
        g_service.discovery.join();
    std::lock_guard<std::mutex> lock(g_service.mutex);
    g_service.peers.clear();
    g_service.pending.clear();
    g_service.received.clear();
    g_service.endpoints.clear();
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
                  {"message", ok && devices ? "已更新；按设备 ID 验证身份，自动发现地址并重连，无需因 IP 改变重新配对"
                                            : "操作失败：请检查两个开关、配对码、可选私网地址及设备是否已存在"},
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
