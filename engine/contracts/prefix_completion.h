#pragma once

#include <cstddef>
#include <cstdint>

namespace FanyImePrefixCompletion
{
template <typename Char> bool Numeric(const Char *raw, std::size_t length)
{
    if (raw == nullptr || length == 0)
        return false;
    for (std::size_t index = 0; index < length; ++index)
        if (raw[index] < '0' || raw[index] > '9')
            return false;
    return true;
}

// TSF 与 Server 只在已保存数字词条的首位起头；组合中的数字继续输入，不用于选词。
template <typename Char>
bool Accepts(const Char *raw, std::size_t length, std::size_t caret, Char character, std::uint16_t starts)
{
    if (character < '0' || character > '9' || caret > length)
        return false;
    return length == 0 ? (starts & (1u << (character - '0'))) != 0 : Numeric(raw, length);
}

inline bool ValidStarts(const wchar_t *payload)
{
    if (payload == nullptr)
        return false;
    for (unsigned index = 0; index < 10; ++index)
        if (payload[index] != L'0' && payload[index] != L'1')
            return false;
    return payload[10] == L'\0';
}

inline std::uint16_t ParseStarts(const wchar_t *payload)
{
    if (payload == nullptr)
        return 0;
    std::uint16_t mask = 0;
    for (unsigned index = 0; index < 10; ++index)
    {
        if (payload[index] != L'0' && payload[index] != L'1')
            return 0;
        if (payload[index] == L'1')
            mask |= 1u << index;
    }
    return payload[10] == L'\0' ? mask : 0;
}
} // namespace FanyImePrefixCompletion
