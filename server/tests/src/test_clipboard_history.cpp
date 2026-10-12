#include "clipboard/clipboard_history.h"
#include "tests/includes/test_framework.h"

#include <Windows.h>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
class ClipboardFixture
{
  public:
    ClipboardFixture()
    {
        const DWORD size = GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", nullptr, 0);
        if (size)
        {
            previous_.resize(size);
            GetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", previous_.data(), size);
            previous_.resize(size - 1);
        }
        directory_ =
            std::filesystem::temp_directory_path() / (L"msime-剪贴板测试-" + std::to_wstring(GetCurrentProcessId()) +
                                                      std::filesystem::path(ClipboardRecord::GenerateId()).wstring());
        std::filesystem::create_directories(directory_);
        REQUIRE(SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", directory_.c_str()));
        std::ofstream(directory_ / "config.toml") << "[utility]\nclipboard_history = true\n";
    }

    ~ClipboardFixture()
    {
        SetEnvironmentVariableW(L"METASEQUOIA_IME_DATA_DIR", previous_.empty() ? nullptr : previous_.c_str());
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void Write(std::string_view payload)
    {
        std::ofstream output(ClipboardHistory::StorePath(), std::ios::binary | std::ios::trunc);
        output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    std::string Read() const
    {
        std::ifstream input(ClipboardHistory::StorePath(), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    }

  private:
    std::wstring previous_;
    std::filesystem::path directory_;
};
ClipboardRecord::Record RecordAt(size_t index)
{
    return ClipboardHistory::LoadRecords().at(index);
}

std::wstring TextAt(size_t index)
{
    return ClipboardHistory::Load().at(index);
}
} // namespace

TEST_CASE(clipboard_history_migration_persists_order_and_identity)
{
    ClipboardFixture fixture;
    fixture.Write("[\"最新\\n文本\",\"旧文本\"]");
    const auto items = ClipboardHistory::Load();
    REQUIRE_EQ(items.size(), size_t{2});
    REQUIRE_EQ(items[0], std::wstring(L"最新\n文本"));
    REQUIRE_EQ(items[1], std::wstring(L"旧文本"));
    const auto first = ClipboardRecord::Parse(fixture.Read());
    REQUIRE(first.error.empty());
    REQUIRE(!first.migrated_from_v1);
    REQUIRE(!first.device_id.empty());
    REQUIRE_EQ(first.records[0].origin_device, first.device_id);
    REQUIRE_EQ(first.records[0].created_at, int64_t{0}); // no fabricated historical timestamp
    REQUIRE(!first.records[0].id.empty());
    REQUIRE_EQ(first.records[0].hash, ClipboardRecord::HashText(first.records[0].text));
    REQUIRE_EQ(RecordAt(0).id, first.records[0].id);
    REQUIRE_EQ(ClipboardRecord::Parse(fixture.Read()).device_id, first.device_id);
}

TEST_CASE(clipboard_history_add_remove_and_clear_preserve_metadata)
{
    ClipboardFixture fixture;
    REQUIRE(ClipboardHistory::AddText(L"中文"));
    const auto first = RecordAt(0);
    REQUIRE(!first.origin_device.empty());
    REQUIRE(first.created_at > 0);
    REQUIRE(!ClipboardHistory::AddText(L"中文"));
    REQUIRE_EQ(RecordAt(0).id, first.id);
    REQUIRE(ClipboardHistory::AddText(L"second"));
    REQUIRE_EQ(RecordAt(1).id, first.id);
    REQUIRE(ClipboardHistory::RemoveText(L"second"));
    REQUIRE_EQ(RecordAt(0).id, first.id);
    ClipboardHistory::Clear();
    REQUIRE(ClipboardHistory::Load().empty());
    REQUIRE(ClipboardHistory::AddText(L"after clear"));
    REQUIRE_EQ(RecordAt(0).origin_device, first.origin_device);
}

TEST_CASE(clipboard_history_same_text_recopy_publishes_a_new_local_event)
{
    ClipboardFixture fixture;
    std::vector<ClipboardRecord::Record> events;
    ClipboardHistory::SetCaptureObserver([&](const auto &event) { events.push_back(event); });
    const bool initial = ClipboardHistory::AddText(L"中文 😀", true);
    const bool repeated = ClipboardHistory::AddText(L"中文 😀", true);
    const bool history_load = ClipboardHistory::AddText(L"中文 😀");
    ClipboardHistory::SetCaptureObserver({});
    REQUIRE(initial && repeated && !history_load);
    REQUIRE_EQ(events.size(), size_t{2});
    REQUIRE(events[0].id != events[1].id);
    REQUIRE_EQ(ClipboardHistory::LoadRecords().size(), size_t{1});
    REQUIRE_EQ(RecordAt(0).id, events[1].id);
}

TEST_CASE(clipboard_history_preserves_capacity_and_unicode)
{
    ClipboardFixture fixture;
    const std::wstring chinese(ClipboardHistory::kMaxChars, L'中');
    REQUIRE(ClipboardHistory::AddText(chinese));
    REQUIRE_EQ(TextAt(0), chinese);
    REQUIRE(ClipboardHistory::AddText(std::wstring(3999, L'a') + L"\xd83d\xde00"));
    REQUIRE_EQ(TextAt(0), std::wstring(3999, L'a'));
    for (size_t i = 0; i < ClipboardHistory::kMaxItems + 1; ++i)
        REQUIRE(ClipboardHistory::AddText(std::to_wstring(i)));
    const auto items = ClipboardHistory::Load();
    REQUIRE_EQ(items.size(), ClipboardHistory::kMaxItems);
    REQUIRE_EQ(items.front(), std::to_wstring(ClipboardHistory::kMaxItems));
    REQUIRE_EQ(items.back(), std::wstring(L"1"));
}

TEST_CASE(clipboard_history_refuses_to_overwrite_unreadable_or_future_stores)
{
    ClipboardFixture fixture;
    for (const auto *payload : {"[not JSON", "{\"version\":99,\"items\":[]}"})
    {
        fixture.Write(payload);
        REQUIRE(ClipboardHistory::Load().empty());
        REQUIRE(!ClipboardHistory::AddText(L"new"));
        REQUIRE(!ClipboardHistory::RemoveText(L"old"));
        REQUIRE_EQ(fixture.Read(), std::string(payload));
    }
}

TEST_CASE(clipboard_history_replace_failure_keeps_previous_store)
{
    ClipboardFixture fixture;
    REQUIRE(ClipboardHistory::AddText(L"previous"));
    const auto before = fixture.Read();
    // Simulate antivirus/another reader denying replacement. Reads remain allowed.
    const HANDLE reader = CreateFileW(ClipboardHistory::StorePath().c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(reader != INVALID_HANDLE_VALUE);
    const bool saved = ClipboardHistory::AddText(L"new");
    CloseHandle(reader);
    REQUIRE(!saved);
    REQUIRE_EQ(fixture.Read(), before);
    REQUIRE_EQ(TextAt(0), std::wstring(L"previous"));
}

TEST_CASE(clipboard_history_disabled_capture_leaves_history_untouched)
{
    ClipboardFixture fixture;
    REQUIRE(ClipboardHistory::AddText(L"previous"));
    const auto before = fixture.Read();
    std::ofstream(ClipboardHistory::StorePath().parent_path() / "config.toml")
        << "[utility]\nclipboard_history = false\n";
    REQUIRE(!ClipboardHistory::AddText(L"ignored"));
    REQUIRE_EQ(fixture.Read(), before);
}

TEST_CASE(clipboard_history_import_persists_remote_metadata_without_rebroadcast)
{
    using ClipboardSync::ImportResult;
    ClipboardFixture fixture;
    REQUIRE(ClipboardHistory::AddText(L"local"));
    const auto local = RecordAt(0);
    const ClipboardRecord::Record remote{"remote-id", "from peer", ClipboardRecord::HashText("from peer"), "peer", 123};
    REQUIRE_EQ(ClipboardHistory::ImportRecord(remote, "peer"), ImportResult::Imported);
    const auto received = RecordAt(0);
    REQUIRE_EQ(received.id, remote.id);
    REQUIRE_EQ(received.origin_device, remote.origin_device);
    REQUIRE_EQ(received.created_at, remote.created_at);
    REQUIRE_EQ(TextAt(0), std::wstring(L"from peer"));
    REQUIRE(!ClipboardSync::IsPublishableLocalCapture(received, local.origin_device));
    REQUIRE(ClipboardSync::IsPublishableLocalCapture(RecordAt(1), local.origin_device));
    const auto before = fixture.Read();
    REQUIRE_EQ(ClipboardHistory::ImportRecord(remote, "peer"), ImportResult::Duplicate);
    REQUIRE_EQ(fixture.Read(), before);
    REQUIRE_EQ(ClipboardHistory::ImportRecord(remote, "impostor"), ImportResult::UnauthorizedOrigin);
    REQUIRE_EQ(fixture.Read(), before);
}

TEST_CASE(clipboard_history_remote_import_honors_disable_and_write_failure)
{
    using ClipboardSync::ImportResult;
    ClipboardFixture fixture;
    REQUIRE(ClipboardHistory::AddText(L"local"));
    const auto before = fixture.Read();
    const ClipboardRecord::Record remote{"remote-id", "remote", ClipboardRecord::HashText("remote"), "peer", 123};
    const HANDLE reader = CreateFileW(ClipboardHistory::StorePath().c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(reader != INVALID_HANDLE_VALUE);
    const auto result = ClipboardHistory::ImportRecord(remote, "peer");
    CloseHandle(reader);
    REQUIRE_EQ(result, ImportResult::StoreUnavailable);
    REQUIRE_EQ(fixture.Read(), before);
    std::ofstream(ClipboardHistory::StorePath().parent_path() / "config.toml")
        << "[utility]\nclipboard_history = false\n";
    REQUIRE_EQ(ClipboardHistory::ImportRecord(remote, "peer"), ImportResult::Disabled);
    REQUIRE_EQ(fixture.Read(), before);
}

TEST_CASE(clipboard_history_import_refuses_unreadable_store)
{
    ClipboardFixture fixture;
    fixture.Write("{broken");
    const ClipboardRecord::Record remote{"remote-id", "remote", ClipboardRecord::HashText("remote"), "peer", 123};
    REQUIRE_EQ(ClipboardHistory::ImportRecord(remote, "peer"), ClipboardSync::ImportResult::StoreUnavailable);
    REQUIRE_EQ(fixture.Read(), std::string("{broken"));
}
