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
