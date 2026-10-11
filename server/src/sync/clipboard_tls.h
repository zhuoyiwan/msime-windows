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
// TLS 1.3 external PSK with ephemeral ECDHE, no 0-RTT or session resumption.
// The PSK is a full CSPRNG-generated 256-bit pairing secret, never a user PIN.
struct TlsCallbacks
{
    std::function<std::optional<std::string>(const std::string &key_id)> lookup;
    std::function<bool(const std::string &key_id, const std::string &device)> authorize;
    std::function<std::vector<MsimeClipboardSync::Record>(const std::string &device)> outgoing;
    std::function<bool(const MsimeClipboardSync::Record &, const std::string &device)> receive;
    std::function<void(const std::string &device)> acknowledged;
};

bool IsLanAddress(const std::string &address);
std::string RandomHex(size_t bytes);

class TlsListener
{
  public:
    TlsListener();
    ~TlsListener();
    TlsListener(const TlsListener &) = delete;
    TlsListener &operator=(const TlsListener &) = delete;
    bool Open(const std::string &address, unsigned short port);
    unsigned short Port() const;
    // Polls for one connection for at most 50 ms; an exchange has a total 3-second deadline.
    bool Poll(const std::string &local_device, const TlsCallbacks &callbacks, const std::atomic<bool> &stop);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool TlsExchange(const std::string &address, unsigned short port, const MsimeClipboardLan::PairingCode &peer,
                 const std::string &local_device, const TlsCallbacks &callbacks, const std::atomic<bool> &stop);
} // namespace ClipboardSync
