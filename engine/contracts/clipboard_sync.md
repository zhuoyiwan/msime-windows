# Clipboard event contract v1

The executable C++17 contract is [clipboard_sync.h](clipboard_sync.h). It is independent of the
engine runtime, Windows headers, local JSON storage and any network library. Windows uses this
same definition for stored records and incoming events; a future Android adapter must use these
limits and the golden vector below.

## Transport boundary

These are **plaintext event frames inside an authenticated encrypted connection**. Hosts must
complete device pairing and mutually authenticate the connection before passing any frame to
Decode or importing its record. The authenticated peer ID comes from the paired connection,
never from an untrusted announcement or this frame.

Do not send these bytes on an unencrypted TCP socket. Connection establishment, encryption,
timeouts, replay protection, acknowledgements and discovery are separate work still to implement.
Duplicate history suppression alone is not transport replay protection: once an item is evicted,
its event must still be protected against replay by the connection/session layer.
Adding this contract does not enable a listener, discovery service or pairing flow.

## Frame

All integers use unsigned big-endian encoding. The timestamp must be at most INT64_MAX and
represents Unix seconds, with 0 meaning an unknown historical time. No struct memory is copied
to the wire.

| Offset | Bytes | Field |
|---|---|---|
| 0 | 4 | Body length, excluding the prefix itself |
| 4 | 4 | ASCII magic MSCB |
| 8 | 2 | Major version, exactly 1 |
| 10 | 2 | Opcode, exactly 1 (clipboard event) |
| 12 | 8 | created_at |
| 20 | 2 | ID byte length |
| 22 | 2 | Origin device ID byte length |
| 24 | 4 | Text byte length |
| 28 | 32 | SHA-256 digest of the exact UTF-8 text bytes |
| 60 | variable | ID, origin device ID, then UTF-8 text |

The receiver reads four bytes first and calls FrameSize before allocating the remaining buffer.
A complete frame is at most 12,316 bytes. Truncation, extra bytes, unknown versions/opcodes and
inconsistent lengths are rejected. Decode clears its output on failure.

- IDs are 1–128 bytes, using ASCII letters, digits, dot, underscore or hyphen.
- Text is nonempty, valid UTF-8 without U+0000, at most 4000 UTF-16 units / 12,000 UTF-8 bytes.
  Supplementary characters count as two units. Overlong UTF-8, surrogates and values above
  U+10FFFF are rejected; incoming events are not silently truncated or assigned new IDs.
- The digest becomes 64 lowercase hexadecimal characters in the record. Decode preserves the
  digest; the receiving host must recompute SHA-256 and reject mismatches before changing history.

The local store remains a version-2 JSON object. Its device_id is local store identity and is not
part of a clipboard event. Store and event version numbers are independent.

## Receiver policy

Windows server/src/sync/clipboard_receive_policy.cpp verifies the paired peer ID against
origin_device and rejects forwarding for v1. An event originating locally is a reflection.
Identical IDs and complete records are duplicates; the same ID with different metadata/content
is a conflict. Equal text and hash under another ID is also suppressed, preserving the existing
record and order.

Accepted events prepend to history by receipt order and retain remote metadata. Remote timestamps
do not determine sorting, so an incorrect future clock cannot pin an entry. History remains
bounded to 50 entries. No received event modifies the system clipboard or triggers publication.
Only records captured locally with a positive timestamp are eligible for later publication;
migrated historical text is not automatically broadcast.

ClipboardHistory::ImportRecord applies this policy under the existing store mutex and writes
atomically. It respects the capture switch and leaves unreadable stores unchanged. Its caller must
already have authenticated the peer. This API is ready for the future transport.

## Cross-language golden vector

Input: ID id, origin peer, text hello, timestamp 42.
SHA-256: 2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824.

Frame, concatenated hexadecimal:

    000000434d53434200010001000000000000002a0002000400000005
    2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824
    69647065657268656c6c6f

server/tests/src/test_clipboard_sync.cpp checks this exact vector, malformed frame boundaries,
Unicode capacity, digest verification, peer binding, identity conflicts, reflection and retention.
It runs in both the portable sanitizer target and the full Windows Server test target.
