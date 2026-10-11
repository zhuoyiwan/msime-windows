#pragma once

#include "sync/clipboard_pairing.h"

#include <string>

namespace ClipboardSync
{
// The default is the product listener; tests can isolate both address and port.
void StartLanService(unsigned short port = MsimeClipboardLan::kPort, const std::string &bind_address = "0.0.0.0",
                     unsigned short discovery_port = MsimeClipboardLan::kDiscoveryPort);
void StopLanService();
PairingStore OpenPairingStore();
int64_t UnixNow();
// JSON for Settings. Only an invite response exposes the freshly generated pairing code;
// device lists never expose credentials. Actions: status, invite, cancel, pair, remove.
std::string SettingsRequest(const std::string &action, const std::string &code = {}, const std::string &address = {},
                            const std::string &device = {});
} // namespace ClipboardSync
