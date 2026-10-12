#pragma once

#include "clipboard/clipboard_record.h"
#include "sync/clipboard_receive_policy.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ClipboardHistory
{
constexpr size_t kMaxItems = MsimeClipboardSync::kMaxItems;
constexpr size_t kMaxChars = MsimeClipboardSync::kMaxTextUnits;

std::filesystem::path StorePath();
std::vector<ClipboardRecord::Record> LoadRecords();
std::string LocalDeviceId();
ClipboardSync::ImportResult ImportRecord(const ClipboardRecord::Record &record, std::string_view authenticated_peer);
std::vector<std::wstring> Load();
bool AddText(std::wstring text, bool fresh_copy = false);
bool RemoveText(const std::wstring &text);
void Clear();
bool IsEnabled();
bool IsSyncEnabled();
// Called only after a successful new LOCAL capture. Imports and history loads never notify.
void SetCaptureObserver(std::function<void(const ClipboardRecord::Record &)> observer);
inline constexpr wchar_t kSyncResetEvent[] = L"Local\\MetasequoiaIme.ClipboardSyncReset";
void ResetSyncQueue();
bool SetEnabled(bool enabled);
std::filesystem::file_time_type StoreWriteTime();
std::filesystem::file_time_type ConfigWriteTime();
} // namespace ClipboardHistory

namespace ClipboardMonitor
{
void DisableForThisProcess();
void Start();
void Stop();
void Sync(bool enabled);
bool IsRunning();
// 已认证的新事件进入系统剪贴板；由监听窗口写入并抑制自动回传。
void ApplyRemoteText(std::wstring text);
} // namespace ClipboardMonitor
