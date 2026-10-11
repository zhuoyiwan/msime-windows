#pragma once

#include "clipboard_sync.h"

#include <string>
#include <string_view>

namespace MsimeClipboardLan
{
inline constexpr unsigned short kPort = 49876;
inline constexpr size_t kMaxPeers = 8;
inline constexpr size_t kKeyIdBytes = 16;
inline constexpr size_t kSecretBytes = 32;
inline constexpr int kInviteSeconds = 300;
inline constexpr std::string_view kHello{"MSCL\x00\x01", 6};

inline bool IsHex(std::string_view text, size_t bytes)
{
    if (text.size() != bytes * 2)
        return false;
    for (const char ch : text)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    return true;
}

struct PairingCode
{
    std::string device;
    std::string key_id;
    std::string secret; // 256 random bits, lowercase hex; never log this value.
};

inline std::string EncodePairingCode(const PairingCode &code)
{
    if (!MsimeClipboardSync::ValidIdentity(code.device) || !IsHex(code.key_id, kKeyIdBytes) ||
        !IsHex(code.secret, kSecretBytes))
        return {};
    return "msime1:" + code.device + ":" + code.key_id + ":" + code.secret;
}

inline bool DecodePairingCode(std::string_view text, PairingCode &code)
{
    code = {};
    if (text.size() > 240 || text.substr(0, 7) != "msime1:")
        return false;
    text.remove_prefix(7);
    const auto first = text.find(':');
    if (first == std::string_view::npos)
        return false;
    const auto second = text.find(':', first + 1);
    if (second == std::string_view::npos)
        return false;
    PairingCode candidate{std::string(text.substr(0, first)), std::string(text.substr(first + 1, second - first - 1)),
                          std::string(text.substr(second + 1))};
    if (EncodePairingCode(candidate).empty())
        return false;
    code = std::move(candidate);
    return true;
}
} // namespace MsimeClipboardLan
