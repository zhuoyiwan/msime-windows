#pragma once

#include "clipboard_sync.h"

#include <string>
#include <string_view>

namespace MsimeClipboardLan
{
inline constexpr unsigned short kPort = 49876;
inline constexpr unsigned short kDiscoveryPort = 49877;
inline constexpr size_t kMaxPeers = 8;
inline constexpr size_t kKeyIdBytes = 16;
inline constexpr size_t kSecretBytes = 32;
inline constexpr int kInviteSeconds = 300;
inline constexpr std::string_view kHello{"MSCL\x00\x01", 6};
inline constexpr std::string_view kDiscoveryHello{"MSCD\x00\x01", 6};
inline constexpr size_t kDiscoveryTagBytes = 32;
inline constexpr size_t kDiscoveryMaxBytes = 107 + 2 * MsimeClipboardSync::kMaxIdentityBytes;

enum class DiscoveryType : unsigned char
{
    Request = 1,
    Reply = 2
};

struct DiscoveryMessage
{
    DiscoveryType type = DiscoveryType::Request;
    std::string nonce; // 16 random bytes as lowercase hex; echoed only in its reply.
    std::string key_id;
    std::string from;
    std::string to;
    unsigned short port = kPort; // TCP endpoint hint, authenticated again through TLS.
};

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

// The host appends HMAC-SHA256 over this entire canonical body using the raw pairing secret.
// No pairing secret or clipboard text is carried in discovery packets.
inline std::string DiscoveryBody(const DiscoveryMessage &message)
{
    if ((message.type != DiscoveryType::Request && message.type != DiscoveryType::Reply) ||
        !IsHex(message.nonce, kKeyIdBytes) || !IsHex(message.key_id, kKeyIdBytes) || !message.port ||
        !MsimeClipboardSync::ValidIdentity(message.from) || !MsimeClipboardSync::ValidIdentity(message.to) ||
        message.from == message.to)
        return {};
    std::string body(kDiscoveryHello);
    body.push_back(static_cast<char>(message.type));
    body += message.nonce;
    body += message.key_id;
    body.push_back(static_cast<char>(message.port >> 8));
    body.push_back(static_cast<char>(message.port & 255));
    body.push_back(static_cast<char>(message.from.size()));
    body.push_back(static_cast<char>(message.to.size()));
    body += message.from;
    body += message.to;
    return body;
}

inline bool DecodeDiscoveryBody(std::string_view body, DiscoveryMessage &message)
{
    message = {};
    constexpr size_t fixed = 75;
    if (body.size() < fixed + 2 || body.size() + kDiscoveryTagBytes > kDiscoveryMaxBytes ||
        body.substr(0, 6) != kDiscoveryHello)
        return false;
    const auto from_size = static_cast<unsigned char>(body[73]);
    const auto to_size = static_cast<unsigned char>(body[74]);
    if (body.size() != fixed + from_size + to_size)
        return false;
    DiscoveryMessage candidate{static_cast<DiscoveryType>(static_cast<unsigned char>(body[6])),
                               std::string(body.substr(7, 32)),
                               std::string(body.substr(39, 32)),
                               std::string(body.substr(fixed, from_size)),
                               std::string(body.substr(fixed + from_size, to_size)),
                               static_cast<unsigned short>((static_cast<unsigned char>(body[71]) << 8) |
                                                           static_cast<unsigned char>(body[72]))};
    if (DiscoveryBody(candidate) != body)
        return false;
    message = std::move(candidate);
    return true;
}
} // namespace MsimeClipboardLan
