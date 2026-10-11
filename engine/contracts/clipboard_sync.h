#pragma once

// Clipboard event framing only. A host MUST authenticate the peer and encrypt the connection
// before carrying these frames. Discovery announcements alone never authorize a clipboard import.
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace MsimeClipboardSync
{
inline constexpr uint16_t kVersion = 1;
inline constexpr size_t kMaxItems = 50;
inline constexpr size_t kMaxTextUnits = 4000;
inline constexpr size_t kMaxTextBytes = kMaxTextUnits * 3;
inline constexpr size_t kMaxIdentityBytes = 128;
inline constexpr size_t kFixedFrameBytes = 60;
inline constexpr size_t kMaxFrameBytes = kFixedFrameBytes + 2 * kMaxIdentityBytes + kMaxTextBytes;

struct Record
{
    std::string id;
    std::string text; // UTF-8, no embedded NUL
    std::string hash; // lowercase SHA-256 hex of text, checked by the receiving host
    std::string origin_device;
    int64_t created_at = 0; // Unix seconds, 0 for unknown historical time
};

// Also used by the local-store parser. It accepts U+0000; wire records separately disallow NUL.
inline bool ReadUtf8Character(std::string_view text, size_t &index, size_t &units)
{
    if (index >= text.size())
        return false;
    const auto first = static_cast<unsigned char>(text[index]);
    size_t length = 1;
    uint32_t code = first;
    if (first >= 0xc2 && first <= 0xdf)
    {
        length = 2;
        code = first & 0x1f;
    }
    else if (first >= 0xe0 && first <= 0xef)
    {
        length = 3;
        code = first & 0x0f;
    }
    else if (first >= 0xf0 && first <= 0xf4)
    {
        length = 4;
        code = first & 0x07;
    }
    else if (first >= 0x80)
        return false;
    if (index + length > text.size())
        return false;
    for (size_t i = 1; i < length; ++i)
    {
        const auto next = static_cast<unsigned char>(text[index + i]);
        if ((next & 0xc0) != 0x80)
            return false;
        code = (code << 6) | (next & 0x3f);
    }
    if ((length == 2 && code < 0x80) || (length == 3 && code < 0x800) || (length == 4 && code < 0x10000) ||
        code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))
        return false;
    index += length;
    units = code >= 0x10000 ? 2 : 1;
    return true;
}

inline bool ValidIdentity(std::string_view identity)
{
    if (identity.empty() || identity.size() > kMaxIdentityBytes)
        return false;
    for (const auto ch : identity)
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
              ch == '_' || ch == '.'))
            return false;
    return true;
}

inline bool ValidRecord(const Record &record)
{
    if (!ValidIdentity(record.id) || !ValidIdentity(record.origin_device) || record.created_at < 0 ||
        record.text.empty() || record.text.size() > kMaxTextBytes || record.text.find('\0') != std::string::npos ||
        record.hash.size() != 64)
        return false;
    for (const auto ch : record.hash)
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return false;
    size_t total = 0;
    for (size_t index = 0, units = 0; index < record.text.size();)
    {
        if (!ReadUtf8Character(record.text, index, units))
            return false;
        total += units;
        if (total > kMaxTextUnits)
            return false;
    }
    return true;
}

namespace detail
{
inline void AppendInteger(std::string &out, uint64_t value, size_t bytes)
{
    for (size_t i = bytes; i > 0; --i)
        out.push_back(static_cast<char>((value >> ((i - 1) * 8)) & 0xff));
}

inline uint64_t ReadInteger(std::string_view input, size_t offset, size_t bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i)
        value = (value << 8) | static_cast<unsigned char>(input[offset + i]);
    return value;
}

inline unsigned HexDigit(char ch)
{
    return ch <= '9' ? static_cast<unsigned>(ch - '0') : static_cast<unsigned>(ch - 'a' + 10);
}
} // namespace detail

// Read the 4-byte big-endian length prefix before allocating a receive buffer. It excludes itself.
inline bool FrameSize(std::string_view prefix, size_t &bytes)
{
    bytes = 0;
    if (prefix.size() < 4)
        return false;
    const auto body = detail::ReadInteger(prefix, 0, 4);
    if (body < kFixedFrameBytes - 4 || body > kMaxFrameBytes - 4)
        return false;
    bytes = static_cast<size_t>(body) + 4;
    return true;
}

inline bool Encode(const Record &record, std::string &frame)
{
    frame.clear();
    if (!ValidRecord(record))
        return false;
    detail::AppendInteger(
        frame, kFixedFrameBytes - 4 + record.id.size() + record.origin_device.size() + record.text.size(), 4);
    frame += "MSCB";
    detail::AppendInteger(frame, kVersion, 2);
    detail::AppendInteger(frame, 1, 2); // Clipboard event; unknown opcodes are rejected.
    detail::AppendInteger(frame, static_cast<uint64_t>(record.created_at), 8);
    detail::AppendInteger(frame, record.id.size(), 2);
    detail::AppendInteger(frame, record.origin_device.size(), 2);
    detail::AppendInteger(frame, record.text.size(), 4);
    for (size_t i = 0; i < record.hash.size(); i += 2)
        frame.push_back(
            static_cast<char>((detail::HexDigit(record.hash[i]) << 4) | detail::HexDigit(record.hash[i + 1])));
    frame += record.id;
    frame += record.origin_device;
    frame += record.text;
    return true;
}

inline bool Decode(std::string_view frame, Record &record)
{
    record = {};
    size_t total = 0;
    if (!FrameSize(frame, total) || total != frame.size() || frame.substr(4, 4) != "MSCB" ||
        detail::ReadInteger(frame, 8, 2) != kVersion || detail::ReadInteger(frame, 10, 2) != 1)
        return false;
    const auto created_at = detail::ReadInteger(frame, 12, 8);
    const auto id_size = detail::ReadInteger(frame, 20, 2);
    const auto origin_size = detail::ReadInteger(frame, 22, 2);
    const auto text_size = detail::ReadInteger(frame, 24, 4);
    if (created_at > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()) || id_size > kMaxIdentityBytes ||
        origin_size > kMaxIdentityBytes || text_size > kMaxTextBytes ||
        kFixedFrameBytes + id_size + origin_size + text_size != total)
        return false;
    Record parsed;
    parsed.created_at = static_cast<int64_t>(created_at);
    constexpr char digits[] = "0123456789abcdef";
    for (size_t i = 28; i < kFixedFrameBytes; ++i)
    {
        const auto byte = static_cast<unsigned char>(frame[i]);
        parsed.hash.push_back(digits[byte >> 4]);
        parsed.hash.push_back(digits[byte & 15]);
    }
    size_t offset = kFixedFrameBytes;
    parsed.id = std::string(frame.substr(offset, id_size));
    offset += static_cast<size_t>(id_size);
    parsed.origin_device = std::string(frame.substr(offset, origin_size));
    offset += static_cast<size_t>(origin_size);
    parsed.text = std::string(frame.substr(offset, text_size));
    if (!ValidRecord(parsed))
        return false;
    record = std::move(parsed);
    return true;
}
} // namespace MsimeClipboardSync
