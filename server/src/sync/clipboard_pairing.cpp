#include "sync/clipboard_pairing.h"
#include "sync/clipboard_tls.h"
#include "clipboard/clipboard_record.h"
#include "config/credential_protection.h"

#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <utility>

namespace
{
using Json = nlohmann::json;
struct Lock
{
    HANDLE handle = CreateMutexW(nullptr, FALSE, L"Local\\MetasequoiaIme.ClipboardPairing");
    bool locked = false;
    Lock()
    {
        if (handle)
        {
            const auto result = WaitForSingleObject(handle, 5000);
            locked = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }
    ~Lock()
    {
        if (locked)
            ReleaseMutex(handle);
        if (handle)
            CloseHandle(handle);
    }
};

struct State
{
    std::vector<ClipboardSync::PairedDevice> devices;
    std::string invite_id;
    std::string invite_secret;
    int64_t expires = 0;
};

bool Read(const std::filesystem::path &path, const std::string &local_device, State &state)
{
    if (path.empty() || !MsimeClipboardSync::ValidIdentity(local_device))
        return false;
    std::error_code error;
    if (!std::filesystem::exists(path, error))
        return !error;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() < 0 || input.tellg() > 65536)
        return false;
    std::string ciphertext(static_cast<size_t>(input.tellg()), '\0');
    input.seekg(0);
    if (!input.read(ciphertext.data(), static_cast<std::streamsize>(ciphertext.size())) ||
        !CredentialProtection::IsProtected(ciphertext))
        return false;
    const auto plaintext = CredentialProtection::Unprotect(ciphertext);
    if (!plaintext)
        return false;
    try
    {
        const auto json = Json::parse(*plaintext);
        if (json.at("version") != 1 || json.at("local_device") != local_device || !json.at("devices").is_array() ||
            json.at("devices").size() > MsimeClipboardLan::kMaxPeers)
            return false;
        std::set<std::string> ids, keys;
        for (const auto &item : json.at("devices"))
        {
            ClipboardSync::PairedDevice peer{{item.at("device").get<std::string>(),
                                              item.at("key_id").get<std::string>(),
                                              item.at("secret").get<std::string>()},
                                             item.at("address").get<std::string>()};
            if (MsimeClipboardLan::EncodePairingCode(peer.key).empty() || peer.key.device == local_device ||
                (!peer.address.empty() && !ClipboardSync::IsLanAddress(peer.address)) ||
                !ids.insert(peer.key.device).second || !keys.insert(peer.key.key_id).second)
                return false;
            state.devices.push_back(std::move(peer));
        }
        state.invite_id = json.at("invite_id").get<std::string>();
        state.invite_secret = json.at("invite_secret").get<std::string>();
        state.expires = json.at("expires").get<int64_t>();
        if (state.invite_id.empty())
            return state.invite_secret.empty() && state.expires == 0;
        return MsimeClipboardLan::IsHex(state.invite_id, MsimeClipboardLan::kKeyIdBytes) &&
               MsimeClipboardLan::IsHex(state.invite_secret, MsimeClipboardLan::kSecretBytes) &&
               keys.count(state.invite_id) == 0 && state.expires > 0;
    }
    catch (...)
    {
        return false;
    }
}

bool Write(const std::filesystem::path &path, const std::string &local_device, const State &state)
{
    Json json{{"version", 1},
              {"local_device", local_device},
              {"devices", Json::array()},
              {"invite_id", state.invite_id},
              {"invite_secret", state.invite_secret},
              {"expires", state.expires}};
    for (const auto &peer : state.devices)
        json["devices"].push_back({{"device", peer.key.device},
                                   {"key_id", peer.key.key_id},
                                   {"secret", peer.key.secret},
                                   {"address", peer.address}});
    const auto encrypted = CredentialProtection::Protect(json.dump());
    return encrypted && ClipboardRecord::WriteFileAtomic(path, *encrypted);
}

void ClearInvite(State &state)
{
    state.invite_id.clear();
    state.invite_secret.clear();
    state.expires = 0;
}
} // namespace

ClipboardSync::PairingStore::PairingStore(std::filesystem::path path, std::string local_device)
    : path_(std::move(path)), local_device_(std::move(local_device))
{
}

std::optional<std::vector<ClipboardSync::PairedDevice>> ClipboardSync::PairingStore::Devices() const
{
    Lock lock;
    State state;
    if (!lock.locked || !Read(path_, local_device_, state))
        return std::nullopt;
    return state.devices;
}

std::string ClipboardSync::PairingStore::CreateInvite(int64_t now) const
{
    Lock lock;
    State state;
    if (!lock.locked || now <= 0 || now > (std::numeric_limits<int64_t>::max)() - MsimeClipboardLan::kInviteSeconds ||
        !Read(path_, local_device_, state) || state.devices.size() >= MsimeClipboardLan::kMaxPeers)
        return {};
    state.invite_id = RandomHex(MsimeClipboardLan::kKeyIdBytes);
    state.invite_secret = RandomHex(MsimeClipboardLan::kSecretBytes);
    state.expires = now + MsimeClipboardLan::kInviteSeconds;
    const auto code = MsimeClipboardLan::EncodePairingCode({local_device_, state.invite_id, state.invite_secret});
    return !code.empty() && Write(path_, local_device_, state) ? code : std::string{};
}

bool ClipboardSync::PairingStore::CancelInvite() const
{
    Lock lock;
    State state;
    if (!lock.locked || !Read(path_, local_device_, state))
        return false;
    ClearInvite(state);
    return Write(path_, local_device_, state);
}

bool ClipboardSync::PairingStore::AddPeer(const std::string &code, const std::string &address) const
{
    MsimeClipboardLan::PairingCode key;
    if (!MsimeClipboardLan::DecodePairingCode(code, key) || key.device == local_device_ ||
        (!address.empty() && !IsLanAddress(address)))
        return false;
    Lock lock;
    State state;
    if (!lock.locked || !Read(path_, local_device_, state) || state.devices.size() >= MsimeClipboardLan::kMaxPeers ||
        key.key_id == state.invite_id)
        return false;
    for (const auto &peer : state.devices)
        if (peer.key.device == key.device || peer.key.key_id == key.key_id)
            return false; // revoke explicitly before re-pairing; never silently replace a credential
    state.devices.push_back({std::move(key), address});
    return Write(path_, local_device_, state);
}

bool ClipboardSync::PairingStore::RemovePeer(const std::string &device) const
{
    Lock lock;
    State state;
    if (!lock.locked || !Read(path_, local_device_, state))
        return false;
    const auto before = state.devices.size();
    state.devices.erase(std::remove_if(state.devices.begin(), state.devices.end(),
                                       [&](const auto &peer) { return peer.key.device == device; }),
                        state.devices.end());
    return state.devices.size() != before && Write(path_, local_device_, state);
}

std::optional<std::string> ClipboardSync::PairingStore::Lookup(const std::string &key_id, int64_t now) const
{
    Lock lock;
    State state;
    if (!lock.locked || !Read(path_, local_device_, state))
        return std::nullopt;
    for (const auto &peer : state.devices)
        if (peer.key.key_id == key_id)
            return peer.key.secret;
    if (!state.invite_id.empty() && key_id == state.invite_id &&
        now >= state.expires - MsimeClipboardLan::kInviteSeconds && now < state.expires)
        return state.invite_secret;
    return std::nullopt;
}

bool ClipboardSync::PairingStore::Authorize(const std::string &key_id, const std::string &device, int64_t now) const
{
    if (!MsimeClipboardSync::ValidIdentity(device) || device == local_device_)
        return false;
    Lock lock;
    State state;
    if (!lock.locked || !Read(path_, local_device_, state))
        return false;
    for (const auto &peer : state.devices)
        if (peer.key.key_id == key_id || peer.key.device == device)
            return peer.key.key_id == key_id && peer.key.device == device;
    if (state.invite_id.empty() || key_id != state.invite_id ||
        now < state.expires - MsimeClipboardLan::kInviteSeconds || now >= state.expires ||
        state.devices.size() >= MsimeClipboardLan::kMaxPeers)
        return false;
    state.devices.push_back({{device, state.invite_id, state.invite_secret}, {}});
    ClearInvite(state); // Consume the invitation only after a complete authenticated TLS handshake.
    return Write(path_, local_device_, state);
}

std::optional<std::string> ClipboardSync::PairingStore::DiscoverySecret(const std::string &key_id,
                                                                        const std::string &device, int64_t now) const
{
    if (!MsimeClipboardSync::ValidIdentity(device) || device == local_device_)
        return std::nullopt;
    Lock lock;
    State state;
    if (!lock.locked || !Read(path_, local_device_, state))
        return std::nullopt;
    for (const auto &peer : state.devices)
        if (peer.key.key_id == key_id || peer.key.device == device)
            return peer.key.key_id == key_id && peer.key.device == device ? std::optional<std::string>(peer.key.secret)
                                                                          : std::nullopt;
    // Locating a valid invitation does not consume it or create a trusted peer.
    if (!state.invite_id.empty() && key_id == state.invite_id &&
        now >= state.expires - MsimeClipboardLan::kInviteSeconds && now < state.expires)
        return state.invite_secret;
    return std::nullopt;
}

bool ClipboardSync::PairingStore::RememberAddress(const std::string &key_id, const std::string &device,
                                                  const std::string &address) const
{
    if (!IsLanAddress(address))
        return false;
    Lock lock;
    State state;
    if (!lock.locked || !Read(path_, local_device_, state))
        return false;
    for (auto &peer : state.devices)
        if (peer.key.key_id == key_id && peer.key.device == device)
        {
            if (peer.address == address)
                return true;
            peer.address = address;
            return Write(path_, local_device_, state);
        }
    return false;
}
