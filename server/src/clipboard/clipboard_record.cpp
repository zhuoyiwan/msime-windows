#include "clipboard/clipboard_record.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>

namespace
{
// ---------------------------------------------------------------- SHA-256

constexpr uint32_t kSha256Round[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

inline uint32_t RotateRight(uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

std::string Sha256Hex(std::string_view data)
{
    uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

    std::vector<uint8_t> message(data.begin(), data.end());
    const uint64_t bit_length = static_cast<uint64_t>(data.size()) * 8u;
    message.push_back(0x80);
    while (message.size() % 64u != 56u)
        message.push_back(0x00);
    for (int shift = 56; shift >= 0; shift -= 8)
        message.push_back(static_cast<uint8_t>((bit_length >> shift) & 0xffu));

    for (size_t block = 0; block < message.size(); block += 64)
    {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
        {
            const size_t at = block + static_cast<size_t>(i) * 4u;
            w[i] = (static_cast<uint32_t>(message[at]) << 24) | (static_cast<uint32_t>(message[at + 1]) << 16) |
                   (static_cast<uint32_t>(message[at + 2]) << 8) | static_cast<uint32_t>(message[at + 3]);
        }
        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = RotateRight(w[i - 15], 7) ^ RotateRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = RotateRight(w[i - 2], 17) ^ RotateRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i)
        {
            const uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = h + s1 + ch + kSha256Round[i] + w[i];
            const uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }

    char hex[65] = {};
    for (int i = 0; i < 8; ++i)
        std::snprintf(hex + i * 8, 9, "%08x", state[i]);
    return std::string(hex, 64);
}

// ---------------------------------------------------------------- minimal JSON

struct JsonValue
{
    enum class Kind
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    Kind kind = Kind::Null;
    bool boolean = false;
    int64_t integer = 0;
    std::string text;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object;

    const JsonValue *Find(std::string_view key) const
    {
        for (const auto &entry : object)
        {
            if (entry.first == key)
                return &entry.second;
        }
        return nullptr;
    }
};

constexpr int kMaxJsonDepth = 24;

using MsimeClipboardSync::ReadUtf8Character;

bool ValidUtf8(std::string_view text)
{
    for (size_t index = 0, units = 0; index < text.size();)
        if (!ReadUtf8Character(text, index, units))
            return false;
    return true;
}

class JsonParser
{
  public:
    explicit JsonParser(std::string_view input) : input_(input)
    {
    }

    bool Parse(JsonValue &out, std::string &error)
    {
        SkipWhitespace();
        if (!ParseValue(out, error, 0))
            return false;
        SkipWhitespace();
        if (pos_ != input_.size())
        {
            error = "trailing data after JSON value";
            return false;
        }
        return true;
    }

  private:
    std::string_view input_;
    size_t pos_ = 0;
    size_t nodes_ = 0;

    void SkipWhitespace()
    {
        while (pos_ < input_.size())
        {
            const char ch = input_[pos_];
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool ParseValue(JsonValue &out, std::string &error, int depth)
    {
        if (++nodes_ > 10000)
        {
            error = "too many JSON values";
            return false;
        }
        if (depth > kMaxJsonDepth)
        {
            error = "JSON nesting too deep";
            return false;
        }
        if (pos_ >= input_.size())
        {
            error = "unexpected end of JSON";
            return false;
        }

        switch (input_[pos_])
        {
        case '{':
            return ParseObject(out, error, depth);
        case '[':
            return ParseArray(out, error, depth);
        case '"':
            out.kind = JsonValue::Kind::String;
            return ParseString(out.text, error);
        case 't':
            return ParseLiteral("true", out, true, error);
        case 'f':
            return ParseLiteral("false", out, false, error);
        case 'n':
            return ParseLiteral("null", out, false, error);
        default:
            return ParseNumber(out, error);
        }
    }

    bool ParseLiteral(std::string_view literal, JsonValue &out, bool value, std::string &error)
    {
        if (input_.substr(pos_, literal.size()) != literal)
        {
            error = "invalid JSON literal";
            return false;
        }
        pos_ += literal.size();
        out.kind = literal == "null" ? JsonValue::Kind::Null : JsonValue::Kind::Bool;
        out.boolean = value;
        return true;
    }

    bool ParseNumber(JsonValue &out, std::string &error)
    {
        const size_t start = pos_;
        if (pos_ < input_.size() && input_[pos_] == '-')
            ++pos_;
        if (pos_ >= input_.size() || input_[pos_] < '0' || input_[pos_] > '9')
        {
            error = "invalid JSON number";
            return false;
        }
        if (input_[pos_] == '0')
            ++pos_;
        else
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9')
                ++pos_;
        if (pos_ < input_.size() && input_[pos_] == '.')
        {
            ++pos_;
            const size_t digits = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9')
                ++pos_;
            if (pos_ == digits)
            {
                error = "missing fraction digits";
                return false;
            }
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E'))
        {
            ++pos_;
            if (pos_ < input_.size() && (input_[pos_] == '-' || input_[pos_] == '+'))
                ++pos_;
            const size_t digits = pos_;
            while (pos_ < input_.size() && input_[pos_] >= '0' && input_[pos_] <= '9')
                ++pos_;
            if (pos_ == digits)
            {
                error = "missing exponent digits";
                return false;
            }
        }

        out.kind = JsonValue::Kind::Number;
        out.text = std::string(input_.substr(start, pos_ - start));
        const auto converted = std::from_chars(out.text.data(), out.text.data() + out.text.size(), out.integer);
        if (converted.ec != std::errc{} || converted.ptr != out.text.data() + out.text.size())
            out.integer = -1; // versions and timestamps require representable integers
        return true;
    }

    static void AppendUtf8(std::string &out, uint32_t code_point)
    {
        if (code_point <= 0x7fu)
        {
            out.push_back(static_cast<char>(code_point));
        }
        else if (code_point <= 0x7ffu)
        {
            out.push_back(static_cast<char>(0xc0u | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        }
        else if (code_point <= 0xffffu)
        {
            out.push_back(static_cast<char>(0xe0u | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0u | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80u | ((code_point >> 12) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((code_point >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (code_point & 0x3fu)));
        }
    }

    bool ParseHex4(uint32_t &out, std::string &error)
    {
        if (pos_ + 4 > input_.size())
        {
            error = "truncated \\u escape";
            return false;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char ch = input_[pos_++];
            value <<= 4;
            if (ch >= '0' && ch <= '9')
                value |= static_cast<uint32_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                value |= static_cast<uint32_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                value |= static_cast<uint32_t>(ch - 'A' + 10);
            else
            {
                error = "invalid \\u escape";
                return false;
            }
        }
        out = value;
        return true;
    }

    bool ParseString(std::string &out, std::string &error)
    {
        if (pos_ >= input_.size() || input_[pos_] != '"')
        {
            error = "expected string";
            return false;
        }
        ++pos_;
        out.clear();
        while (pos_ < input_.size())
        {
            const char ch = input_[pos_++];
            if (ch == '"')
            {
                if (ValidUtf8(out))
                    return true;
                error = "invalid UTF-8 string";
                return false;
            }
            if (ch != '\\')
            {
                if (static_cast<unsigned char>(ch) < 0x20)
                {
                    error = "unescaped control character";
                    return false;
                }
                out.push_back(ch);
                continue;
            }
            if (pos_ >= input_.size())
            {
                error = "truncated escape";
                return false;
            }
            const char escape = input_[pos_++];
            switch (escape)
            {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                uint32_t first = 0;
                if (!ParseHex4(first, error))
                    return false;
                uint32_t code_point = first;
                if (first >= 0xd800u && first <= 0xdbffu)
                {
                    if (pos_ + 1 < input_.size() && input_[pos_] == '\\' && input_[pos_ + 1] == 'u')
                    {
                        const size_t second_start = pos_;
                        pos_ += 2;
                        uint32_t second = 0;
                        if (!ParseHex4(second, error))
                            return false;
                        if (second >= 0xdc00u && second <= 0xdfffu)
                            code_point = 0x10000u + ((first - 0xd800u) << 10) + (second - 0xdc00u);
                        else
                        {
                            code_point = 0xfffdu;
                            pos_ = second_start; // preserve a following non-surrogate escape
                        }
                    }
                    else
                    {
                        code_point = 0xfffdu; // lone high surrogate
                    }
                }
                else if (first >= 0xdc00u && first <= 0xdfffu)
                {
                    code_point = 0xfffdu; // lone low surrogate
                }
                AppendUtf8(out, code_point);
                break;
            }
            default:
                error = "unknown escape";
                return false;
            }
        }
        error = "unterminated string";
        return false;
    }

    bool ParseArray(JsonValue &out, std::string &error, int depth)
    {
        ++pos_; // '['
        out.kind = JsonValue::Kind::Array;
        SkipWhitespace();
        if (pos_ < input_.size() && input_[pos_] == ']')
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            JsonValue element;
            SkipWhitespace();
            if (!ParseValue(element, error, depth + 1))
                return false;
            out.array.push_back(std::move(element));
            SkipWhitespace();
            if (pos_ >= input_.size())
            {
                error = "unterminated array";
                return false;
            }
            const char ch = input_[pos_++];
            if (ch == ',')
                continue;
            if (ch == ']')
                return true;
            error = "expected ',' or ']'";
            return false;
        }
    }

    bool ParseObject(JsonValue &out, std::string &error, int depth)
    {
        ++pos_; // '{'
        out.kind = JsonValue::Kind::Object;
        SkipWhitespace();
        if (pos_ < input_.size() && input_[pos_] == '}')
        {
            ++pos_;
            return true;
        }
        while (true)
        {
            SkipWhitespace();
            if (out.object.size() >= 64)
            {
                error = "too many JSON object fields";
                return false;
            }
            std::string key;
            if (!ParseString(key, error))
                return false;
            SkipWhitespace();
            if (pos_ >= input_.size() || input_[pos_] != ':')
            {
                error = "expected ':'";
                return false;
            }
            ++pos_;
            SkipWhitespace();
            JsonValue value;
            if (!ParseValue(value, error, depth + 1))
                return false;
            if (out.Find(key))
            {
                error = "duplicate object key";
                return false;
            }
            out.object.emplace_back(std::move(key), std::move(value));
            SkipWhitespace();
            if (pos_ >= input_.size())
            {
                error = "unterminated object";
                return false;
            }
            const char ch = input_[pos_++];
            if (ch == ',')
                continue;
            if (ch == '}')
                return true;
            error = "expected ',' or '}'";
            return false;
        }
    }
};

// ---------------------------------------------------------------- helpers

void AppendEscaped(std::string &out, std::string_view text)
{
    out.push_back('"');
    for (unsigned char ch : text)
    {
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (ch < 0x20u)
            {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                out += buffer;
            }
            else
            {
                out.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    out.push_back('"');
}

// Preserve the same text capacity on Windows and in the portable target.
std::string ClampUtf8(std::string value, size_t limit, bool &clamped)
{
    size_t index = 0, total = 0;
    while (index < value.size())
    {
        const size_t start = index;
        size_t units = 0;
        if (!ReadUtf8Character(value, index, units) || total + units > limit)
        {
            clamped = true;
            value.resize(start);
            break;
        }
        total += units;
    }
    return value;
}

std::string ReadField(const JsonValue &object, std::string_view key)
{
    const JsonValue *value = object.Find(key);
    if (value == nullptr || value->kind != JsonValue::Kind::String)
        return {};
    return value->text;
}

int64_t ReadInteger(const JsonValue &object, std::string_view key)
{
    const JsonValue *value = object.Find(key);
    if (value == nullptr || value->kind != JsonValue::Kind::Number)
        return 0;
    return value->integer;
}

ClipboardRecord::Record RecordFromObject(const JsonValue &object, bool &text_clamped)
{
    ClipboardRecord::Record record;
    record.id = ReadField(object, "id");
    record.text = ClampUtf8(ReadField(object, "text"), ClipboardRecord::kMaxTextUnits, text_clamped);
    record.origin_device = ReadField(object, "origin_device");
    record.created_at = std::max(int64_t{0}, ReadInteger(object, "created_at"));
    record.hash = ClipboardRecord::HashText(record.text);
    if (record.id.empty())
        record.id = ClipboardRecord::GenerateId();
    return record;
}
} // namespace

namespace ClipboardRecord
{
std::string HashText(std::string_view text)
{
    return Sha256Hex(text);
}

std::string GenerateId()
{
    thread_local std::mt19937_64 generator(std::random_device{}());
    static const char *digits = "0123456789abcdef";
    uint64_t words[2] = {generator(), generator()};
    std::string out;
    out.reserve(32);
    for (uint64_t word : words)
    {
        for (int i = 0; i < 16; ++i)
            out.push_back(digits[(word >> (i * 4)) & 0xfu]);
    }
    return out;
}

std::string Serialize(const std::vector<Record> &records, std::string_view device_id)
{
    std::string out;
    out += "{\n  \"version\": ";
    out += std::to_string(kFormatVersion);
    out += ",\n  \"device_id\": ";
    AppendEscaped(out, device_id);
    out += ",\n  \"items\": [";
    for (size_t i = 0; i < records.size(); ++i)
    {
        const Record &record = records[i];
        out += i == 0 ? "\n    {" : ",\n    {";
        out += "\"id\": ";
        AppendEscaped(out, record.id);
        out += ", \"text\": ";
        AppendEscaped(out, record.text);
        out += ", \"hash\": ";
        AppendEscaped(out, record.hash);
        out += ", \"origin_device\": ";
        AppendEscaped(out, record.origin_device);
        out += ", \"created_at\": ";
        out += std::to_string(record.created_at);
        out += "}";
    }
    out += records.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

ParseResult Parse(std::string_view payload)
{
    ParseResult result;

    if (payload.size() > kMaxPayloadBytes)
    {
        result.error = "clipboard store exceeds size limit";
        return result;
    }

    JsonParser parser(payload);
    JsonValue root;
    std::string error;
    if (!parser.Parse(root, error))
    {
        result.error = error;
        return result;
    }

    if (root.kind == JsonValue::Kind::Array)
    {
        // Legacy v1: a bare array of strings.
        result.migrated_from_v1 = true;
        for (const JsonValue &element : root.array)
        {
            if (element.kind != JsonValue::Kind::String)
            {
                result.dropped_invalid = true;
                continue;
            }
            if (result.records.size() >= kMaxItems)
            {
                result.truncated = true;
                break;
            }
            bool clamped = false;
            Record record;
            record.text = ClampUtf8(element.text, kMaxTextUnits, clamped);
            if (clamped)
                result.truncated = true;
            if (record.text.empty() || record.text.find('\0') != std::string::npos)
            {
                result.dropped_invalid = true;
                continue;
            }
            record.id = GenerateId();
            record.hash = HashText(record.text);
            result.records.push_back(std::move(record));
        }
        return result;
    }

    if (root.kind != JsonValue::Kind::Object)
    {
        result.error = "root is neither an array nor an object";
        return result;
    }

    const JsonValue *version = root.Find("version");
    if (version == nullptr || version->kind != JsonValue::Kind::Number || version->integer != kFormatVersion)
    {
        result.error = "unsupported or missing clipboard store version";
        return result;
    }

    result.device_id = ReadField(root, "device_id");
    if (result.device_id.size() > 128 || result.device_id.find('\0') != std::string::npos)
    {
        result.error = "invalid local device identity";
        return result;
    }

    const JsonValue *items = root.Find("items");
    if (items == nullptr || items->kind != JsonValue::Kind::Array)
    {
        result.error = "missing items array";
        return result;
    }

    for (const JsonValue &element : items->array)
    {
        if (element.kind != JsonValue::Kind::Object)
        {
            result.dropped_invalid = true;
            continue;
        }
        if (result.records.size() >= kMaxItems)
        {
            result.truncated = true;
            break;
        }
        bool clamped = false;
        Record record = RecordFromObject(element, clamped);
        if (ReadField(element, "id") != record.id || ReadField(element, "hash") != record.hash)
            result.repaired = true;
        if (clamped)
            result.truncated = true;
        if (record.text.empty() || record.text.find('\0') != std::string::npos || record.id.size() > 128 ||
            record.origin_device.size() > 128 || record.id.find('\0') != std::string::npos ||
            record.origin_device.find('\0') != std::string::npos)
        {
            result.dropped_invalid = true;
            continue;
        }
        result.records.push_back(std::move(record));
    }
    return result;
}

bool WriteFileAtomic(const std::filesystem::path &path, std::string_view payload)
{
    if (path.empty())
        return false;
    std::error_code error;
    const std::filesystem::path directory = path.parent_path();
    if (!directory.empty())
        std::filesystem::create_directories(directory, error);
    if (error)
        return false;

    std::filesystem::path temporary = path;
    temporary += "." + GenerateId() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
            return false;
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        output.flush();
        output.close();
        if (!output)
        {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }

    std::filesystem::rename(temporary, path, error);
    if (!error)
        return true;

    // Never remove the last good store on failure (including sharing violations on Windows).
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}
} // namespace ClipboardRecord
