#include "clipboard/clipboard_record.h"
#include "engine/contracts/clipboard_sync.h"
#include "sync/clipboard_receive_policy.h"
#include "tests/includes/test_framework.h"

#include <string>
#include <vector>

namespace
{
using ClipboardSync::ImportResult;
using MsimeClipboardSync::Record;

Record Event(std::string id = "id", std::string text = "hello", std::string origin = "peer", int64_t time = 42)
{
    Record record{std::move(id), std::move(text), "", std::move(origin), time};
    record.hash = ClipboardRecord::HashText(record.text);
    return record;
}

std::string Hex(std::string_view value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : value)
    {
        const auto ch = static_cast<unsigned char>(byte);
        out.push_back(digits[ch >> 4]);
        out.push_back(digits[ch & 15]);
    }
    return out;
}
} // namespace

TEST_CASE(clipboard_sync_frame_matches_cross_platform_golden_vector)
{
    const auto event = Event();
    std::string frame;
    REQUIRE(MsimeClipboardSync::Encode(event, frame));
    REQUIRE_EQ(Hex(frame), std::string("000000434d53434200010001000000000000002a0002000400000005"
                                       "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
                                       "69647065657268656c6c6f"));
    Record decoded;
    REQUIRE(MsimeClipboardSync::Decode(frame, decoded));
    REQUIRE_EQ(decoded.id, event.id);
    REQUIRE_EQ(decoded.origin_device, event.origin_device);
    REQUIRE_EQ(decoded.created_at, event.created_at);
    REQUIRE_EQ(decoded.text, event.text);
    REQUIRE_EQ(decoded.hash, event.hash);
}

TEST_CASE(clipboard_sync_frame_rejects_truncation_unknown_headers_and_invalid_lengths)
{
    std::string frame;
    REQUIRE(MsimeClipboardSync::Encode(Event(), frame));
    Record output;
    for (size_t length = 0; length < frame.size(); ++length)
    {
        REQUIRE(!MsimeClipboardSync::Decode(std::string_view(frame).substr(0, length), output));
        REQUIRE(output.id.empty());
    }
    REQUIRE(!MsimeClipboardSync::Decode(frame + "extra", output));
    for (const auto offset : {size_t{0}, size_t{4}, size_t{9}, size_t{11}, size_t{12}, size_t{20}, size_t{24}})
    {
        auto bad = frame;
        bad[offset] = static_cast<char>(0xff);
        REQUIRE(!MsimeClipboardSync::Decode(bad, output));
    }
    size_t size = 1;
    REQUIRE(!MsimeClipboardSync::FrameSize(std::string(4, '\xff'), size));
    REQUIRE_EQ(size, size_t{0});
    REQUIRE(!MsimeClipboardSync::FrameSize(std::string(4, '\0'), size));
    REQUIRE(MsimeClipboardSync::FrameSize(std::string_view(frame).substr(0, 4), size));
    REQUIRE_EQ(size, frame.size());
}

TEST_CASE(clipboard_sync_unicode_capacity_and_invalid_records)
{
    std::string chinese;
    for (size_t i = 0; i < MsimeClipboardSync::kMaxTextUnits; ++i)
        chinese += "中";
    std::string frame;
    REQUIRE(MsimeClipboardSync::Encode(Event("id", chinese), frame));
    Record decoded;
    REQUIRE(MsimeClipboardSync::Decode(frame, decoded));
    REQUIRE_EQ(decoded.text, chinese);
    REQUIRE(!MsimeClipboardSync::Encode(Event("id", chinese + "中"), frame));
    REQUIRE(!MsimeClipboardSync::Encode(Event("id", "\xed\xa0\x80"), frame)); // raw surrogate
    REQUIRE(!MsimeClipboardSync::Encode(Event("id", std::string("a\0b", 3)), frame));
    REQUIRE(!MsimeClipboardSync::Encode(Event("id", "text", "peer", -1), frame));
    REQUIRE(!MsimeClipboardSync::Encode(Event("../bad id"), frame));
    REQUIRE(!MsimeClipboardSync::Encode(Event("id", "text", ""), frame));
    REQUIRE(!MsimeClipboardSync::Encode(Event(std::string(129, 'a')), frame));
    auto bad = Event();
    bad.hash = std::string(64, 'z');
    REQUIRE(!MsimeClipboardSync::Encode(bad, frame));
}

TEST_CASE(clipboard_sync_peer_binding_hash_check_and_reflection_do_not_mutate_history)
{
    std::vector<Record> history{Event("local-id", "local", "self")};
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event(), "different-peer", "self"),
               ImportResult::UnauthorizedOrigin);
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, history[0], "peer", "self"), ImportResult::Reflected);
    auto corrupted = Event();
    corrupted.hash = std::string(64, '0');
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, corrupted, "peer", "self"), ImportResult::Invalid);
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event(), "", "self"), ImportResult::Invalid);
    REQUIRE_EQ(history.size(), size_t{1});
    REQUIRE_EQ(history[0].text, std::string("local"));
}

TEST_CASE(clipboard_sync_duplicate_event_and_identity_conflict_keep_existing_metadata)
{
    std::vector<Record> history;
    const auto event = Event();
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, event, "peer", "self"), ImportResult::Imported);
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, event, "peer", "self"), ImportResult::Duplicate);
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event("id", "changed"), "peer", "self"),
               ImportResult::IdentityConflict);
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event("id", "hello", "peer", 999), "peer", "self"),
               ImportResult::IdentityConflict);
    REQUIRE_EQ(history.size(), size_t{1});
    REQUIRE_EQ(history[0].id, event.id);
    REQUIRE_EQ(history[0].created_at, event.created_at);
    REQUIRE_EQ(history[0].text, event.text);
}

TEST_CASE(clipboard_sync_same_text_new_event_is_a_fresh_copy)
{
    std::vector<Record> history;
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event(), "peer", "self"), ImportResult::Imported);
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event("different-id"), "peer", "self"), ImportResult::Imported);
    REQUIRE_EQ(history.size(), size_t{1});
    REQUIRE_EQ(history[0].id, std::string("different-id"));
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event("different-id"), "peer", "self"), ImportResult::Duplicate);
}

TEST_CASE(clipboard_sync_bounds_history_and_orders_by_receipt)
{
    std::vector<Record> history;
    REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event("future", "future", "peer", 9999999999), "peer", "self"),
               ImportResult::Imported);
    for (size_t i = 0; i < MsimeClipboardSync::kMaxItems; ++i)
    {
        const auto text = std::to_string(i);
        REQUIRE_EQ(ClipboardSync::MergeReceived(history, Event("id-" + text, text, "peer", 1), "peer", "self"),
                   ImportResult::Imported);
    }
    REQUIRE_EQ(history.size(), MsimeClipboardSync::kMaxItems);
    REQUIRE_EQ(history.front().text, std::string("49"));
    REQUIRE_EQ(history.back().text, std::string("0"));
}

TEST_CASE(clipboard_sync_exports_only_new_local_captures)
{
    REQUIRE(ClipboardSync::IsPublishableLocalCapture(Event("id", "new", "self"), "self"));
    REQUIRE(!ClipboardSync::IsPublishableLocalCapture(Event("id", "remote", "peer"), "self"));
    REQUIRE(!ClipboardSync::IsPublishableLocalCapture(Event("id", "migrated", "self", 0), "self"));
    auto bad = Event("id", "text", "self");
    bad.hash = std::string(64, '0');
    REQUIRE(!ClipboardSync::IsPublishableLocalCapture(bad, "self"));
}
