# Clipboard LAN transport v1

`clipboard_lan.h` owns the constants and pairing-code codec. Event frames remain defined by
`clipboard_sync.h`; do not duplicate either format in hosts. This transport is separate from
the local history file's version 2.

## Pairing

The inviter generates a 16-byte random key ID and 32-byte random secret using a cryptographic
RNG. The displayed code is `msime1:<inviter-device>:<32 lowercase hex key ID>:<64 lowercase hex secret>`.
Device IDs follow `clipboard_sync.h`'s identity grammar; colons are disallowed. The recipient
transfers the code through a trusted channel and separately supplies the inviter's private IPv4.
The code is a credential, not a short human PIN. It must never appear in discovery packets or logs.
Windows capture skips clipboard text containing `msime1:` so copying a code does not share it.

The inviter stores one outstanding invitation, expiring after 300 seconds. Generating another
or cancelling invalidates the previous one. After an authenticated TLS handshake and encrypted
device hello, the inviter atomically binds the key to that device and consumes the invitation.
The same key then authenticates reconnections from that device only. Maximum eight devices;
re-pairing requires explicit revocation first. The recipient stores the inviter identity, secret
and private IP before connecting; its device list therefore describes saved trust, not connectivity.

Windows stores the entire versioned pairing document in `clipboard_peers.dat`, encrypted using
DPAPI for the current Windows user. Invalid/foreign/corrupt documents fail closed and are not
overwritten. Revocation removes the key; the service rechecks authorization before importing each
event. Previously imported history is retained. Pairing is bilateral: revoke on both devices to
remove both copies. A failed or timed-out attempt may have completed pairing on only the inviter;
the recipient automatically retries using its saved key.

## TLS profile

TCP port **49876**, private IPv4/loopback/link-local addresses only, no DNS resolution.
TLS **1.3 only**, external PSK identity = the 32-character key ID (ASCII), PSK = the raw 32-byte
secret. Cipher = **TLS_AES_128_GCM_SHA256**, key exchange = **psk_dhe_ke**, groups X25519 or P-256.
No `psk_ke`, certificates, fallback TLS versions, session tickets, resumption cache, or early data.
Every exchange performs a fresh ephemeral key exchange. OpenSSL handles authentication, AEAD,
traffic-key derivation, sequence numbers and on-path replay protection. Historical ID deduplication
is an additional application policy, not the transport replay mechanism.

Android can implement this profile using a TLS implementation supporting TLS 1.3 external PSKs
and DHE; ordinary browser fetch/WebSocket and Android's default SSLSocket are not sufficient.
Android integration is still pending; this specifies the interoperation contract rather than
claiming that an Android client is already available.

## Encrypted transcript

1. Client sends hello: six bytes `4d 53 43 4c 00 01`, one unsigned byte device-ID length
   (1–128), followed by device-ID ASCII.
2. Server binds/checks that client identity against the authenticated key and replies with
   the same hello format containing its own device ID. Client must match its saved inviter ID.
3. Client sends a batch: one unsigned byte count (0–50), then that many complete MSCB frames,
   including each frame's four-byte length prefix. Server imports each using the authenticated
   client identity; any rejected frame aborts the exchange.
4. Server replies with its batch in the same format; client imports against the authenticated
   server identity.
5. Client sends one acknowledgement byte `01`. Server then considers its batch delivered.
   Client considers its batch delivered after validating the server batch and sending this byte.

Before allocating a frame, validate the length prefix with `FrameSize()` (maximum 12,316 bytes).
Network I/O shares a three-second deadline across connect/handshake/hello/batch/ack; storage callbacks
remain subject to the host mutex and filesystem limits. A stopped
service interrupts I/O polling within 50 ms. No application record is accepted before both TLS
authentication and identity binding. Rejected/missing acknowledgements retain queued events for
retry; the receiver deduplicates repeated records. Partial batches can already have imported valid
records, so the exchange is not an all-or-nothing transaction.

The recipient initiates duplex exchanges about once per second. Both endpoints queue only fresh
successful local captures made while sharing is enabled and the peer is already known; they do not
backfill stored history. Each peer's volatile queue retains at most 50 events. Imports never enqueue
outbound events, and neither endpoint sets the system clipboard. Queues clear when sharing is
disabled or the service stops; no persistent offline delivery is promised.

The Windows installer creates a program-specific inbound rule for this port, **private network
profiles and local subnet only**, and removes it on uninstall. Runtime listening additionally
requires both `utility.clipboard_history` and `utility.clipboard_lan_sync`, defaulting to false.
Manual IP is the first implementation; discovery and IPv6 are pending. IP changes require re-pairing.
