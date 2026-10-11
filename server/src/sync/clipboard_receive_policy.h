#pragma once

#include "engine/contracts/clipboard_sync.h"

#include <string_view>
#include <vector>

namespace ClipboardSync
{
enum class ImportResult
{
    Imported,
    Duplicate,
    Reflected,
    Invalid,
    IdentityConflict,
    UnauthorizedOrigin,
    Disabled,
    StoreUnavailable
};

// authenticated_peer is established by a paired encrypted connection, not supplied by the frame.
// v1 accepts direct captures only; forwarding another device's events is deliberately unsupported.
ImportResult MergeReceived(std::vector<MsimeClipboardSync::Record> &history, const MsimeClipboardSync::Record &incoming,
                           std::string_view authenticated_peer, std::string_view local_device);

// Publish only new local captures. Migrated historical entries (timestamp 0) and imported entries
// never become outbound events; receiving an event does not trigger another broadcast.
bool IsPublishableLocalCapture(const MsimeClipboardSync::Record &record, std::string_view local_device);
} // namespace ClipboardSync
