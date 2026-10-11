#pragma once

#include "engine/contracts/clipboard_lan.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ClipboardSync
{
struct DiscoveryDestination
{
    std::string address;
    unsigned short port = MsimeClipboardLan::kDiscoveryPort;
};

struct DiscoveredPeer
{
    std::string device;
    std::string key_id;
    std::string address;
    unsigned short port = MsimeClipboardLan::kPort;
};

using DiscoveryLookup = std::function<std::optional<std::string>(const std::string &key_id, const std::string &device)>;

std::string SignDiscovery(const MsimeClipboardLan::DiscoveryMessage &message, const std::string &secret);
bool VerifyDiscovery(std::string_view packet, const std::string &secret, MsimeClipboardLan::DiscoveryMessage &message);
std::vector<DiscoveryDestination> LanBroadcasts(unsigned short port);

// Single worker owns this object. Endpoint hints never replace a credential and must
// complete TLS identity verification before being saved or used for clipboard imports.
class LanDiscovery
{
  public:
    explicit LanDiscovery(std::string local_device);
    ~LanDiscovery();
    LanDiscovery(const LanDiscovery &) = delete;
    LanDiscovery &operator=(const LanDiscovery &) = delete;
    bool Open(const std::string &address, unsigned short discovery_port, unsigned short tcp_port);
    unsigned short Port() const;
    bool Request(const MsimeClipboardLan::PairingCode &peer, const std::vector<DiscoveryDestination> &targets);
    std::vector<DiscoveredPeer> Poll(const DiscoveryLookup &lookup, const std::atomic<bool> &stop);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ClipboardSync
