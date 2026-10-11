#include "sync/clipboard_receive_policy.h"
#include "clipboard/clipboard_record.h"

namespace ClipboardSync
{
ImportResult MergeReceived(std::vector<MsimeClipboardSync::Record> &history, const MsimeClipboardSync::Record &incoming,
                           std::string_view authenticated_peer, std::string_view local_device)
{
    if (!MsimeClipboardSync::ValidIdentity(authenticated_peer) || !MsimeClipboardSync::ValidIdentity(local_device) ||
        !MsimeClipboardSync::ValidRecord(incoming) || incoming.hash != ClipboardRecord::HashText(incoming.text))
        return ImportResult::Invalid;
    if (incoming.origin_device == local_device)
        return ImportResult::Reflected;
    if (incoming.origin_device != authenticated_peer || authenticated_peer == local_device)
        return ImportResult::UnauthorizedOrigin;
    for (const auto &record : history)
    {
        if (record.id == incoming.id)
        {
            if (record.origin_device == incoming.origin_device && record.text == incoming.text &&
                record.hash == incoming.hash && record.created_at == incoming.created_at)
                return ImportResult::Duplicate;
            return ImportResult::IdentityConflict;
        }
    }
    for (const auto &record : history)
        if (record.hash == incoming.hash && record.text == incoming.text)
            return ImportResult::Duplicate;
    // Order by receipt, not the remote clock; a peer far in the future must not pin its item.
    history.insert(history.begin(), incoming);
    if (history.size() > MsimeClipboardSync::kMaxItems)
        history.resize(MsimeClipboardSync::kMaxItems);
    return ImportResult::Imported;
}

bool IsPublishableLocalCapture(const MsimeClipboardSync::Record &record, std::string_view local_device)
{
    return MsimeClipboardSync::ValidIdentity(local_device) && record.origin_device == local_device &&
           record.created_at > 0 && MsimeClipboardSync::ValidRecord(record) &&
           record.hash == ClipboardRecord::HashText(record.text);
}
} // namespace ClipboardSync
