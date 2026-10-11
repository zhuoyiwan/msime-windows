// Covers the clipboard store format: the v1 -> v2 migration, the untrusted-input guards, and the
// atomic write. Lives in the portable target, so a sanitizer build reaches it with no vcpkg tree.

#include "clipboard/clipboard_record.h"

#include "tests/includes/test_framework.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using ClipboardRecord::Parse;
using ClipboardRecord::Record;
using ClipboardRecord::Serialize;

namespace
{
std::string ReadAll(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE(clipboard_record_sha256_matches_known_vectors)
{
    REQUIRE_EQ(ClipboardRecord::HashText(""),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    REQUIRE_EQ(ClipboardRecord::HashText("abc"),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    // Multi-block input, to exercise the padding path past a single 64-byte chunk.
    REQUIRE_EQ(ClipboardRecord::HashText(std::string(1000, 'a')),
               std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
}

TEST_CASE(clipboard_record_round_trips_through_serialize)
{
    std::vector<Record> records;
    records.push_back({"id-1", "hello", ClipboardRecord::HashText("hello"), "device-a", 1700000000});
    records.push_back({"id-2", "换行\n和\"引号\"", ClipboardRecord::HashText("换行\n和\"引号\""), "", 1700000001});

    const std::string payload = Serialize(records);
    const auto parsed = Parse(payload);
    REQUIRE(parsed.error.empty());
    REQUIRE(!parsed.migrated_from_v1);
    REQUIRE_EQ(parsed.records.size(), size_t{2});
    REQUIRE_EQ(parsed.records[0].id, std::string("id-1"));
    REQUIRE_EQ(parsed.records[0].text, std::string("hello"));
    REQUIRE_EQ(parsed.records[0].origin_device, std::string("device-a"));
    REQUIRE_EQ(parsed.records[0].created_at, int64_t{1700000000});
    REQUIRE_EQ(parsed.records[1].text, records[1].text);
    // Field order is fixed, so an unchanged store is byte-stable across writes.
    REQUIRE_EQ(Serialize(parsed.records), payload);
}

TEST_CASE(clipboard_record_migrates_legacy_array)
{
    const std::string legacy = "[\"first\", \"second\"]";
    const auto parsed = Parse(legacy);
    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.migrated_from_v1);
    REQUIRE_EQ(parsed.records.size(), size_t{2});
    REQUIRE_EQ(parsed.records[0].text, std::string("first"));
    REQUIRE_EQ(parsed.records[1].text, std::string("second"));
    // Migrated entries must be usable by the sync path, so they get identity and a hash.
    REQUIRE(!parsed.records[0].id.empty());
    REQUIRE_EQ(parsed.records[0].hash, ClipboardRecord::HashText("first"));
    REQUIRE(parsed.records[0].origin_device.empty());

    const std::string empty = "[]";
    const auto empty_parsed = Parse(empty);
    REQUIRE(empty_parsed.migrated_from_v1);
    REQUIRE_EQ(empty_parsed.records.size(), size_t{0});
}

TEST_CASE(clipboard_record_rejects_malformed_payloads)
{
    REQUIRE(!Parse("not json").error.empty());
    REQUIRE(!Parse("{\"version\":2}").error.empty());              // items missing
    REQUIRE(!Parse("{\"version\":2,\"items\":{}}").error.empty()); // items is not an array
    REQUIRE(!Parse("[\"a\"] extra").error.empty());                // trailing data after a valid array
    REQUIRE(!Parse("{\"version\":2,\"items\":[]} trailing").error.empty());
    // Unterminated structures must not be accepted as an empty store.
    REQUIRE(!Parse("{\"version\":2,\"items\":[").error.empty());

    // A store from a newer build must be refused rather than partially read.
    REQUIRE(!Parse("{\"version\":99,\"items\":[]}").error.empty());

    // Non-string and empty entries inside a legacy array are skipped, not fatal.
    const auto mixed = Parse("[\"ok\", 5, \"\", null]");
    REQUIRE(mixed.error.empty());
    REQUIRE_EQ(mixed.records.size(), size_t{1});
    REQUIRE(mixed.dropped_invalid);
}

TEST_CASE(clipboard_record_enforces_limits)
{
    // More than kMaxItems entries: keep the leading ones and say so.
    std::string payload = "{\"version\":2,\"items\":[";
    for (size_t i = 0; i < ClipboardRecord::kMaxItems + 10; ++i)
    {
        if (i != 0)
            payload += ",";
        payload += "{\"id\":\"i" + std::to_string(i) + "\",\"text\":\"t" + std::to_string(i) + "\"}";
    }
    payload += "]}";
    const auto parsed = Parse(payload);
    REQUIRE(parsed.error.empty());
    REQUIRE_EQ(parsed.records.size(), ClipboardRecord::kMaxItems);
    REQUIRE(parsed.truncated);

    // Over-long text is clamped, and clamped on a UTF-8 boundary so the result stays valid.
    std::string big;
    while (big.size() < (ClipboardRecord::kMaxTextUnits + 64) * 3)
        big += "中"; // 3 bytes each
    std::vector<Record> records;
    records.push_back({"id", big, "", "", 0});
    const auto clamped = Parse(Serialize(records));
    REQUIRE(clamped.error.empty());
    REQUIRE_EQ(clamped.records.size(), size_t{1});
    REQUIRE(clamped.truncated);
    REQUIRE_EQ(clamped.records[0].text.size(), ClipboardRecord::kMaxTextUnits * 3);
    // The clamp must not split a code point: every input character here is 3 bytes, so a
    // boundary-correct result is a whole number of them.
    REQUIRE_EQ(clamped.records[0].text.size() % 3, size_t{0});
}

TEST_CASE(clipboard_record_decodes_escapes_and_surrogates)
{
    const auto parsed = Parse("{\"version\":2,\"items\":[{\"id\":\"a\",\"text\":\"\\u4e2d\\ud83d\\ude00\"}]}");
    REQUIRE(parsed.error.empty());
    REQUIRE_EQ(parsed.records.size(), size_t{1});
    REQUIRE_EQ(parsed.records[0].text, std::string("\xe4\xb8\xad\xf0\x9f\x98\x80")); // U+4E2D, U+1F600

    // A lone surrogate is replaced rather than emitted as invalid UTF-8.
    const auto lone = Parse("{\"version\":2,\"items\":[{\"id\":\"a\",\"text\":\"\\ud800\"}]}");
    REQUIRE(lone.error.empty());
    REQUIRE_EQ(lone.records[0].text, std::string("\xef\xbf\xbd"));
    const auto followed = Parse("[\"\\ud800\\u0041\"]");
    REQUIRE(followed.error.empty());
    REQUIRE_EQ(followed.records[0].text, std::string("\xef\xbf\xbd"
                                                     "A"));
}

TEST_CASE(clipboard_record_write_is_atomic_and_readable)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("msime_clipboard_record_test_" + ClipboardRecord::GenerateId());
    std::error_code error;
    const std::filesystem::path store = directory / "clipboard_history.json";

    std::vector<Record> records;
    records.push_back({"id-1", "persisted", ClipboardRecord::HashText("persisted"), "device-a", 42});
    const std::string payload = Serialize(records);

    REQUIRE(ClipboardRecord::WriteFileAtomic(store, payload));
    REQUIRE(std::filesystem::exists(store));
    // The temporary file must not be left behind.
    REQUIRE(!std::filesystem::exists(std::filesystem::path(store.string() + ".tmp")));
    REQUIRE_EQ(ReadAll(store), payload);

    const auto reloaded = Parse(ReadAll(store));
    REQUIRE(reloaded.error.empty());
    REQUIRE_EQ(reloaded.records.size(), size_t{1});
    REQUIRE_EQ(reloaded.records[0].text, std::string("persisted"));

    // Overwriting an existing store must succeed (the rename has to replace it).
    records.push_back({"id-2", "second", ClipboardRecord::HashText("second"), "", 43});
    REQUIRE(ClipboardRecord::WriteFileAtomic(store, Serialize(records)));
    REQUIRE_EQ(Parse(ReadAll(store)).records.size(), size_t{2});

    std::filesystem::remove_all(directory, error);
}

TEST_CASE(clipboard_record_rejects_invalid_json_and_oversized_input)
{
    for (const auto *payload : {"{\"items\":[]}", "{\"version\":\"2\",\"items\":[]}", "{\"version\":2.9,\"items\":[]}",
                                "{\"version\":2e0,\"items\":[]}", "{\"version\":02,\"items\":[]}", "[+1]", "[1.]",
                                "[1e]", "[\"raw\nnewline\"]", "{\"version\":2,\"version\":2,\"items\":[]}"})
        REQUIRE(!Parse(payload).error.empty());
    REQUIRE(!Parse(std::string("[\"") + "\xc0\xaf" + "\"]").error.empty());
    REQUIRE(!Parse(std::string(ClipboardRecord::kMaxPayloadBytes + 1, ' ')).error.empty());
    REQUIRE(Parse("[1.5, 2e3, -42, true, null]").error.empty()); // valid unknown values are skippable
    REQUIRE(!Parse(std::string(30, '[') + "0" + std::string(30, ']')).error.empty());
    std::string nodes = "[0";
    for (size_t i = 0; i < 10000; ++i)
        nodes += ",0";
    REQUIRE(!Parse(nodes + "]").error.empty());
}

TEST_CASE(clipboard_record_preserves_chinese_and_complete_surrogate_pairs)
{
    std::string chinese;
    for (size_t i = 0; i < ClipboardRecord::kMaxTextUnits; ++i)
        chinese += "中";
    auto parsed = Parse(Serialize({{"a", chinese, "", "", 0}}));
    REQUIRE(parsed.error.empty());
    REQUIRE_EQ(parsed.records[0].text, chinese);
    REQUIRE(!parsed.truncated);

    const std::string prefix(ClipboardRecord::kMaxTextUnits - 1, 'a');
    parsed = Parse(Serialize({{"a", prefix + "\xf0\x9f\x98\x80", "", "", 0}}));
    REQUIRE(parsed.error.empty());
    REQUIRE_EQ(parsed.records[0].text, prefix);
    REQUIRE(parsed.truncated);
}

TEST_CASE(clipboard_record_repairs_identity_and_hash_after_clamping)
{
    const auto parsed = Parse(Serialize({{"", std::string(4100, 'a'), "forged", "", -5}}, "device"));
    REQUIRE(parsed.error.empty());
    REQUIRE_EQ(parsed.device_id, std::string("device"));
    REQUIRE(parsed.repaired);
    REQUIRE_EQ(parsed.records[0].id.size(), size_t{32});
    REQUIRE_EQ(parsed.records[0].hash, ClipboardRecord::HashText(parsed.records[0].text));
    REQUIRE_EQ(parsed.records[0].created_at, int64_t{0});
    REQUIRE_EQ(Parse("[\"a\\u0000b\"]").records.size(), size_t{0});
}

TEST_CASE(clipboard_record_failed_replace_preserves_destination)
{
    const auto directory =
        std::filesystem::temp_directory_path() / ("msime_clipboard_failure_" + ClipboardRecord::GenerateId());
    const auto destination = directory / "existing_directory";
    std::filesystem::create_directories(destination);
    REQUIRE(!ClipboardRecord::WriteFileAtomic(destination, "must not replace a directory"));
    REQUIRE(std::filesystem::is_directory(destination));
    size_t children = 0;
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        REQUIRE_EQ(entry.path(), destination);
        ++children;
    }
    REQUIRE_EQ(children, size_t{1}); // failed write cleaned up only its own temporary file
    std::filesystem::remove(destination);
    std::filesystem::remove(directory);
}
