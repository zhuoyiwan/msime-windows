#pragma once

#include "clipboard/clipboard_record.h"
#include "sync/clipboard_receive_policy.h"

#include <filesystem>
#include <string>
#include <vector>

namespace ClipboardHistory
{
constexpr size_t kMaxItems = MsimeClipboardSync::kMaxItems;
constexpr size_t kMaxChars = MsimeClipboardSync::kMaxTextUnits;

std::filesystem::path StorePath();
std::vector<ClipboardRecord::Record> LoadRecords();
ClipboardSync::ImportResult ImportRecord(const ClipboardRecord::Record &record, std::string_view authenticated_peer);
std::vector<std::wstring> Load();
bool AddText(std::wstring text);
bool RemoveText(const std::wstring &text);
void Clear();
bool IsEnabled();
bool SetEnabled(bool enabled);
std::filesystem::file_time_type StoreWriteTime();
std::filesystem::file_time_type ConfigWriteTime();
} // namespace ClipboardHistory

namespace ClipboardMonitor
{
void Start();
void Stop();
void Sync(bool enabled);
bool IsRunning();
} // namespace ClipboardMonitor
