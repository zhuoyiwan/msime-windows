#pragma once

#include "engine/contracts/clipboard_lan.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ClipboardSync
{
struct PairedDevice
{
    MsimeClipboardLan::PairingCode key;
    std::string address; // Last connection hint; identity and trust are keyed by device + key_id.
};

// All operations lock the same named mutex across Server and Settings and fail closed on corruption.
// The entire file is encrypted by DPAPI for the current Windows user, then replaced atomically.
class PairingStore
{
  public:
    PairingStore(std::filesystem::path path, std::string local_device);
    std::optional<std::vector<PairedDevice>> Devices() const;
    std::string CreateInvite(int64_t now) const;
    bool CancelInvite() const;
    bool AddPeer(const std::string &code, const std::string &address) const;
    bool RemovePeer(const std::string &device) const;
    std::optional<std::string> Lookup(const std::string &key_id, int64_t now) const;
    bool Authorize(const std::string &key_id, const std::string &device, int64_t now) const;
    std::optional<std::string> DiscoverySecret(const std::string &key_id, const std::string &device, int64_t now) const;
    // Call only after a complete TLS exchange verifies this exact credential and device ID.
    bool RememberAddress(const std::string &key_id, const std::string &device, const std::string &address) const;

  private:
    std::filesystem::path path_;
    std::string local_device_;
};
} // namespace ClipboardSync
