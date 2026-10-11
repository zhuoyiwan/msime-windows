#pragma once

// Versioned, device-aware storage model for clipboard history.
//
// The on-disk store used to be a bare JSON array of strings, which carries no identity, no
// timestamp and no origin device, so two devices cannot tell "the same entry" from "a new one"
// and cannot suppress echo of what they just received. This module owns the v2 format, the
// migration from the v1 array, and the atomic write used to publish it.
//
// Deliberately free of Windows headers: it parses untrusted input (the store file, and later the
// payloads a peer sends), so it lives in the portable test target where a sanitizer build can
// cover it without a vcpkg tree.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "engine/contracts/clipboard_sync.h"

namespace ClipboardRecord
{
// Bump when the on-disk shape changes in a way older readers cannot handle.
constexpr int kFormatVersion = 2;

// Kept equal to the legacy store limit so migration cannot silently drop entries the old code kept.
constexpr size_t kMaxItems = MsimeClipboardSync::kMaxItems;

// Preserve the legacy 4000 UTF-16 code unit limit, including Chinese text and surrogate pairs.
constexpr size_t kMaxTextUnits = MsimeClipboardSync::kMaxTextUnits;
constexpr size_t kMaxPayloadBytes = 2 * 1024 * 1024;

using Record = MsimeClipboardSync::Record;

struct ParseResult
{
    std::vector<Record> records;
    std::string device_id;         // persistent identity of this local store
    bool migrated_from_v1 = false; // input was the legacy bare array
    bool truncated = false;        // entries beyond kMaxItems were dropped
    bool dropped_invalid = false;  // entries that were not usable strings were skipped
    bool repaired = false;         // missing identity or stale metadata was normalized
    std::string error;             // empty when the payload was at least structurally readable
};

// Lowercase hex SHA-256 of the UTF-8 bytes.
std::string HashText(std::string_view text);

// Random hex identifier, 32 characters.
std::string GenerateId();

// Deterministic field order, so an unchanged store round-trips byte for byte.
std::string Serialize(const std::vector<Record> &records, std::string_view device_id = {});

// Accepts both the v2 object and the legacy v1 array of strings. Malformed input yields an error
// and no records; callers must not overwrite unreadable stores with an empty history.
ParseResult Parse(std::string_view payload);

// temp file + rename, so a reader never observes a half-written store.
bool WriteFileAtomic(const std::filesystem::path &path, std::string_view payload);
} // namespace ClipboardRecord
