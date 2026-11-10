# myvava-control/1 wire contract

This describes the implemented control, event, file and live media channels.
All channels run inside the same authenticated DTLS-PSK session. Device identity
comes from DTLS, never a payload-supplied identity. Capabilities are runtime
values: storage availability, configured NAS, encryption and server archive
configuration must be inspected separately from protocol support.

The station sends confirmable CoAP POST messages to `myvava/v1/sync`; the server
returns CoAP Content responses. CoAP confirmation/retransmission and DTLS replay
protection belong to libcoap/OpenSSL. No HTTP, custom cipher or parallel ACK layer
is used. DTLS endpoint MTU is 1,200 bytes. A control/event exchange and a file exchange
may each be outstanding, with distinct tokens. File responses use libcoap
async separate responses after the storage worker completes; disk waits do
not hold the control/event request slot.
The station sync interval is 50..5,000 ms (default 1,000 ms); online status expires
after 15 seconds without an authenticated sync request.
For two seconds after a command/result exchange, follow-up polls use the smaller
of the configured interval and 100 ms. Result chunks, including the final receipt
confirmation, proceed immediately. Event/telemetry ACKs do not renew this window.

All multibyte integers below are unsigned network byte order. The payload has a
56-byte header and normally 0..768 bytes of data. Unknown magic/version/type, reserved bits,
inconsistent sizes and excess limits are rejected.

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII `MVA1` |
| 4 | 16 | Station boot nonce |
| 20 | 16 | Server process epoch; zero before first sync |
| 36 | 8 | Server command ID; zero when no result exists |
| 44 | 4 | SYNC/ACK: total result bytes; COMMAND: remaining execution TTL in milliseconds |
| 48 | 4 | Result offset; zero for COMMAND/IDLE |
| 52 | 2 | Data length |
| 54 | 1 | SYNC=1, IDLE=2, COMMAND=3, ACK=4, EVENT=5, EVENT_ACK=6; FILE_OFFER=7, FILE_DATA=8, FILE_COMMIT=9, FILE_ACK=10 |
| 55 | 1 | Reserved zero |
| 56 | variable | Command text or a result chunk |

For a new server epoch, only IDLE is accepted; the station clears old-epoch
command/result state before requesting work. A command response must echo the
station boot nonce, have 1..511 bytes of single-line command text, a positive
monotonically increasing ID and a TTL of 1..60,000 ms. The station computes its
execution deadline from the associated request's local start time, not receipt
time. The server currently allocates a 30-second task lifetime.

Only the device loop invokes the shared command dispatcher. A retained repeated
command ID with identical content reuses its result; changed content or an older
ID fails closed. The same boot/epoch remains valid across transport reconnection.
SDK camera commands require `if-serial`; existing version guards remain available.
Command strings are parsed internally and never executed by a shell.

Results are bounded to fewer than 32,768 bytes. The station sends contiguous
chunks, and the server ACKs the accumulated byte offset. Identical duplicate
chunks are accepted without append; conflicting/overlapping lengths or gaps are
rejected. The server commits each accepted chunk to its private SQLite journal
before ACK. The final ACK indicates a complete **persisted command result**, not
confirmed camera application, video upload or event persistence. A later empty
SYNC can request the next command while retaining the previous result.

An elective CoAP option `65004 = 8` on SYNC requests and Content replies negotiates
result batches of at most 8 × 768 = 6,144 bytes. The agent waits for the reply
before using a batch; an absent/unknown option retains 768-byte results. COMMAND,
event and media payload limits are unchanged. CoAP Block1 splits a batch below
the same 1,200-byte DTLS MTU. The server assembles one bounded batch per authenticated
device, verifies session/boot/epoch, request tag, contiguous offsets and identical
duplicates, and expires an incomplete assembly after 60 seconds. The negotiated
result exchange also allows up to 60 seconds for Block1 retransmission backoffs;
other control/event exchanges retain their 20-second deadline. Valid partial
blocks refresh authenticated device presence. A Block1 Continue
acknowledges buffered transport bytes only. The result ACK still follows a full
SQLite commit of that batch; partial results never become completed tasks. Session
deletion and registry reload clear buffered assemblies. Final-ACK loss, request-key
deduplication and uncertain mutation handling retain their original behavior.

Server-side local IPC version 3 (0600 Unix socket, one line per connection):

```text
devices
submit DEVICE_ID BOOT_HEX SERVER_EPOCH_HEX REQUEST_KEY_HEX COMMAND...
result SERVER_EPOCH_HEX COMMAND_ID_HEX
lookup DEVICE_ID REQUEST_KEY_HEX
history DEVICE_ID BEFORE_COMMAND_ID_HEX
events DEVICE_ID BEFORE_EVENT_ID_HEX
```

Boot/epoch/request keys each use 32 lowercase hex digits; command IDs use 16.
For a new submission, concatenate the 16-hex `devices.request_prefix` and a fresh
16-hex random suffix. Retrying must retain the full original key. A missing key
with an expired/unissued prefix is rejected; it is never turned into a new task.
The default prefix window rotates every 60 seconds; the immediately previous
prefix is valid only until 120 seconds from its original issuance. The server
uses a monotonic clock and advertises remaining validity. Prefixes are freshness
markers, not authentication secrets. IPC v1 callers using wholly random keys
must upgrade; station UDP payloads remain compatible.

`devices` returns current epoch, identities, boot IDs, online age, request prefix
and journal capacity/retained/pruned counts. `submit` returns a task ID and its
epoch; identical retained request keys return the existing task. `result`
requires the epoch saved with the original task, never a freshly fetched epoch.
Historical epochs can be read after restart; new submissions still must use the
current process epoch. Both fields are matched, so a historical ID cannot
accidentally identify a different task. Results include state/reason, metadata,
and a JSON-escaped result string. The caller must inspect/parse that result
separately from the transport outcome.

`lookup` recovers a task by device ID and request key after a lost submission
reply. `history` returns at most 32 metadata rows in descending ID order; zero
means newest and `next_before` is an exclusive cursor. Empty pages end traversal.

Limits: 16 provisioned devices, one pending task per device, 1,024 retained tasks
by default (configurable 16..8,192), 8 local administrative clients, and a 5-second
local connection deadline. Only terminal tasks outside active request windows
that can no longer be retransmitted may be pruned, oldest first. For the current
epoch, an empty SYNC with matching ID/total/full offset confirms receipt of the
final ACK and permits recycling that result and earlier terminal tasks for the
same device/boot. A changed station boot or server epoch also retires the old
transfer context. Expired never-dispatched tasks need no result confirmation.
This prevents a lost final ACK from trapping the station in endless retries
after its result has been pruned. Pruning and the new insertion commit atomically;
if protected records fill the quota, new submissions explicitly fail. A pruned
key cannot be reused because its original prefix has expired. Missing/pruned
references return `task_not_retained`; no unlimited result retention is promised.

The server persists acceptance before replying and SENT before dispatching.
On restart it changes epoch, retains completed results, changes queued tasks to
expired, and changes dispatched tasks without complete results to unknown.
It does not redispatch either class. A station reboot similarly invalidates
pending old-boot tasks. Partial result bytes never appear as a completed JSON
result. State reasons distinguish expiry, station restart and server restart.
The journal uses SQLite WAL with synchronous FULL; corruption/unknown schemas
fail closed. SQLite and the task journal are server-only dependencies. This does
not provide exactly-once physical camera effects. The separate event contract follows.

See [runtime guide](../docs/ARCHITECTURE.md) and [integration tests](../tests/sdk_integration.py).

## Persistent event channel (SDK 0.3.0)

A station configured with an event directory sends confirmable POSTs to
`myvava/v1/events` within the same authenticated DTLS session. EVENT contains
160 data bytes, total=160, offset=0 and id=the event sequence. Its MVA1 boot/epoch
identify the current transport session. The event payload has its own **origin**,
which identifies the station process that originally observed the event and
survives spool recovery. Identity is `(DTLS device ID, event origin, sequence)`.
No manufacturer event ID exists for cmd 200: receiving identical raw reports
three times creates three received-report events, not three proven motions.

MVE1 payload integers are big endian; all reserved bytes must be zero:

| Offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | ASCII MVE1 |
| 4 | 16 | Original process random nonce |
| 20 | 8 | Sequence, 1..INT64_MAX; increasing within that origin |
| 28 | 8 | Observation monotonic milliseconds (only comparable within origin) |
| 36 | 8 | Observation Unix milliseconds |
| 44 | 8 | Camera connection generation |
| 52 | 8 | Related recording ID, zero if none |
| 60 | 8 | Cumulative RAM queue drops within origin |
| 68 / 72 | 4 each | Raw result / raw type |
| 76 | 1 | Camera slot 0..3, or 255 for station events |
| 77 | 1 | Kind 1..7, defined below |
| 78 | 1 | Whether station clock was marked valid, 0/1 |
| 79 | 1 | Reserved zero |
| 80 | 32 | Camera serial, zero padded; empty for station events |
| 112 | 48 | Detail code, zero padded |

Texts are bounded ASCII letters, digits, `-_.:` with a terminator and zero
padding. Kinds: 1 station_started, 2 pairing_stage_saved, 3 camera_report_200,
4 pir_wake_report, 5 recording_requested, 6 recording_stop_requested,
7 journal_gap, 8 recording_committed, 9 recording_failed, 10 recording_recovered.
Pair stage uses result_raw (2=intent saved, 3=registration saved).
PIR wake preserves first-connect byte 19. Kinds 5/6 describe recorder requests.
Kind 8 follows file and metadata fsync/rename/directory fsync; kind 9 reports a
failed attempt. Kind 10 reports an existing committed catalog row on startup;
it is not a new recording. These use the captured camera serial/generation and
record ID, recording-start unix_ms, and notification-time monotonic_ms. There
is no upload confirmation here. File commit and event spool commit are separate;
a crash between them is reconciled by catalog recovery, subject to event capacity.

The server commits the event and `(device,origin)` high-water sequence in the
same SQLite transaction **before** EVENT_ACK. ACK has total=160, offset=0,
id=sequence, data length=16 and the original origin nonce as data. The station
only schedules spool removal for an exact matching ACK; unlink + directory fsync
happen on its storage worker. Lost replies and crash recovery may resend events.
Equal high-water sequence requires identical payload. Lower sequences are
acknowledged without reinsertion, including after history pruning. This relies
on ordered sending within each origin: the receiver never supports out-of-order
parallel delivery for one origin. A new random origin prevents sequence reuse
when the station restarts. A fresh server database will ingest whatever pending
spool remains; already acknowledged/deleted station events require server backup.

Event history defaults to 8,192 rows globally (`--event-history-limit 16..65536`);
oldest rows are pruned in the insert transaction. Stream cursors remain after
pruning, at most 65,536 origins globally; reaching that independent limit rejects
new origins without ACK instead of silently resetting deduplication. `devices`
returns event_journal retained/limit/pruned/origins/origin_limit for monitoring.
The current release requires maintenance before the origin ceiling; it has no
automatic cursor expiry or external archival service. `events DEVICE BEFORE`
returns up to 32 newest entries with an exclusive next_before (zero starts;
an empty page ends). Event IDs and sequences are 16 hex digits. Each row exposes
received_at (server wall clock), sequence_gap and the original event. Pruning
is not lossless permanent archival. Per-user unread state and notifications are
not part of this device event transport.

The station spool has 64 RAM entries plus 16..4096 durable entries (default
1024), 164 bytes per file including CRC32, with filesystem block overhead. A
worker writes a private temporary file, fsyncs it, renames and fsyncs the private
directory before publishing it for network transmission. Startup verifies
names, lengths, CRC and payloads, preserves corrupt committed files and reports
an error rather than silently resetting the queue. Known unfinished .tmp files
are discarded; their events were never published or acknowledged. Dedicated
directory and lock prohibit sharing a spool across processes. A symlinked spool
is rejected. No automatic directory creation or fallback to SPI is performed.

Full spools retain existing entries and hold at most 64 more in RAM; subsequent
arrivals increment the drop count and consume sequence numbers. When RAM space
returns a journal_gap summarizes cumulative drops. Missing sequences are also
visible at the server. Neither buffered events nor a not-yet-durable gap marker
survive sudden process/power loss; the exact count of those losses after a crash
is unknown. CRC is corruption detection, not encryption. File and directory
fsync semantics depend on the SD filesystem/controller and need hardware tests.
Initial mount/permission/corruption failures require operator repair and restart;
runtime write failures retain the RAM head and retry. Blocking kernel storage I/O
can still delay the worker and graceful shutdown, though it holds no core mutex
while accessing storage. The OpenWrt launcher still requires the data mount.

Control sync/results have scheduling priority; at most one event is outstanding.
Event store 5.03 is retried after one second without tearing down working control.
Other invalid replies retain the spool and trigger transport recovery. Unanswered
exchanges can delay the shared channel until the existing 20-second timeout;
there are no independent media/transfer streams yet. Server DTLS sessions with no
valid authenticated business sync for 15 seconds are disconnected, including
stalled handshakes after cookie verification, even if raw packets keep arriving. This allows a newly booted device to handshake through a
reused NAT tuple; it does not bypass DTLS authentication.

Schema 2/3 migrates transactionally to schema 4 while preserving task rows
and existing events. Schema 4 adds event classification/filter indexes and
retained clip-state projections; it does not change event wire bytes. IPC v3 adds events and event_journal;
existing v2 control commands keep their syntax. Older 0.2 clients without events
can use the new server. SDK 0.4 recording events require both station and server
0.4; 0.3 cannot decode kinds 8..10. Older servers have no events endpoint.
Downgrade requires a pre-upgrade backup, including the changed recording metadata.


## Reliable file channel

Authenticated CON POST `/myvava/v1/files`, using the same DTLS session and MVA1
header. Identity comes from the session; current synchronized boot/epoch and
current device session are required. Header `id` is the source recording ID,
`total` is complete file bytes, `offset` is the byte position. Maximum file size
is 128 MiB and must be divisible by 188. Only completed TS source files qualify.

OFFER has offset zero and the following 128-byte immutable descriptor. Integers
are unsigned network byte order; string fields are zero-padded.

| Offset | Size | Value |
|---:|---:|---|
| 0 | 4 | `MVR1` |
| 4 | 8 | Recording ID, identical to header |
| 12 | 4 | File bytes, identical to header |
| 16 | 32 | SHA-256 of complete TS file |
| 48 | 32 | Camera SN, 1..31 ASCII bytes using the camera protocol's SN alphabet |
| 80 | 8 | Recording start Unix seconds |
| 88 | 8 | First input timestamp in milliseconds |
| 96 | 8 | Last input timestamp in milliseconds |
| 104 | 4 | Video frame count |
| 108 | 4 | Audio frame count |
| 112 | 4 | Audio sample rate; zero without audio |
| 116 | 1 | Audio channels 0..2 |
| 117 | 1 | Original camera slot 0..3 |
| 118 | 1 | Camera media stream 0..1 |
| 119 | 1 | PIR flag 0/1 |
| 120 | 1 | Wall clock valid flag 0/1 |
| 121 | 1 | Format = 1 (MPEG-TS H.264/AAC) |
| 122 | 6 | Reserved zero |

DATA payload is SHA-256 (32 bytes) followed by 1..736 file bytes. COMMIT payload
is only SHA-256 and its offset equals total. A DATA replay wholly inside the
received prefix is accepted only if its bytes match. Gaps, changed descriptors,
conflicting overlaps and changed identity/digest are rejected.

FILE_ACK echoes boot, epoch, recording ID and total. Its payload is SHA-256 plus
one result byte; header offset is the confirmed receive position:

- 0 partial: received prefix persisted; **not a completed recording**.
- 1 committed: complete file verified, renamed, directory synced and metadata
  commit marker durably published. Offset must equal total.
- 2 identity/descriptor/byte conflict.
- 3 reserved quota or file entry limit exhausted.
- 4 storage I/O unavailable.
- 5 final digest mismatch.
- 6 invalid operation or length/offset.

CoAP 5.03 means storage unavailable/busy or synchronization needed. The station
backs off and re-offers; it never treats an empty CoAP ACK as file completion.
At most one asynchronous file request per authenticated device and 16 total
storage jobs/results are retained. A timed-out response can still correspond
to completed disk work; OFFER reconciles it. Disconnect does not erase partial
files or reset device-qualified recording identity.

Local IPC adds `files DEVICE PAGE` (zero-based pages of 16). Station commands
add identity-guarded `upload-list SLOT [PAGE]`, `nas-list SLOT [PAGE]` and policy
fields `cloud=0|1`, `nas=0|1`. Local record metadata V3 captures destination
eligibility; details, commit ordering and limitations are in
[storage guide](../docs/STORAGE.md). These additions do not implement
remote record-get, browser playback, live media, retention or at-rest encryption.

## 工作树新增实时通道

类型 11 MEDIA_POLL、12 MEDIA_CONTROL、13 MEDIA_FRAME 见 [实时媒体](../docs/LIVE.md) 和 media.h。MEDIA_POLL 的 id/offset 为已消费对讲租约/序号；控制 payload 为四条 48 字节租约（BE u64 ID、SN32、talk1、零7），可选 16 字节对讲头（BE u64 lease、BE u32 seq、slot1、零1、BE u16 长度）和最多512字节 AAC。帧 payload 头32字节为 BE u64 seq/timestamp/generation、slot/audio/key 各1、零5，随后最多736字节数据；MVA1 id 是租约，total/offset 是帧大小/偏移。媒体非可靠碎片不复用文件提交状态；丢帧恢复到关键帧。

## Current storage command lifecycle

Storage commands execute on a four-slot bounded I/O worker. A queued command is
bound to the storage generation observed at admission; checks inside catalog and
journal locks reject an operation after card replacement. The single remote
command lane remains occupied while awaiting the result. Events, files and media
continue independently; an unknown command result must not be blindly repeated.
TF exports and NAS mount operations have their own durable job/status interfaces.

Local event IPC accepts `events DEVICE BEFORE [CATEGORY_OR_DASH SERIAL_OR_DASH]`.
Event JSON adds `category` and a retained `clip_state_reported`; the latter is a
last report, not proof that a physical copy still exists. Web clients resolve the
actual archive/TF source before playback. See MANAGEMENT_SERVER.md.

## Recording query extension (2026-11-07)

Capabilities `record_query`, `record_info_details`, `record_delete_errors`, `record_page_size` and `record_limit` advertise additive command support. `record-query SLOT BEFORE_HEX GENERATION FROM_UNIX TO_UNIX` is read-only and requires the camera serial guard. `0 0` disables the time filter; otherwise the interval is `[FROM, TO)` and excludes untrusted clocks. Start with a zero cursor and generation, then pass `next_before` and `storage_generation`; reset after a station boot change or generation conflict. Results remain bounded to 16 records and use the existing result channel. This is a live cursor, not a frozen snapshot. Details and deletion errors: [interface review](../docs/API.md).

### Batched file exchange and clip previews (2026-11-10)

OFFER/FILE_ACK may carry elective CoAP option 65000 containing one byte `32`.
Only mutual advertisement permits DATA with up to 32 × 736 file bytes (23,552),
plus its 32-byte digest. Missing capability retains the 736-byte exchange.
CoAP Block1 carries the larger logical body using MTU 1200; control/media size
limits remain unchanged. A per-device bounded reassembly verifies session,
boot/epoch, request tag, total, ordered offsets, matching duplicates and a 20s
expiry. Intermediate 2.31 responses acknowledge RAM only. The final FILE_ACK
still follows durable writes, fsync and confirmed offset. Archive capsules remain
736 bytes, preserving existing ciphertext, AAD and resume/repair rules.

`MVR2` is an extended 128-byte descriptor: offsets 0..121 keep their meanings,
122 is archive category (0 unknown, 5 movement, 6 human, 7 face, 8 human+face),
123 is flags (bit 0 camera detection report present, bit 1 positive report,
bit 2 preview), 124..127 is the camera's raw detection type in network order.
A known archive category may exist without a raw camera report (original import).
Unknown fields never imply human/face. MVR1 remains canonical when extension
fields are zero. Existing objects are not rewritten and descriptor conflicts
remain errors. An upgrade must update both peers before producing MVR2.

A preview uses a high-bit export ID, a complete independently hashed TS prefix
ending after the first IDR PES, <= 1 MiB, and one video frame. It has storage
kind `tf_preview`, independent of cloud storage and complete TF exports. The
Server can convert it to JPEG; it must never satisfy a full-clip playback request.
See docs/STORAGE.md for source commands and persistence.


## Media framing 2 and codec epochs (2026-11-10)

An empty media CON poll advertises elective CoAP option **65002**, one byte `2`.
A supporting Server echoes it and sets byte 41 of the corresponding 48-byte
lease record to `2`; bytes 42..47 stay zero. The SDK accepts this only with the
matching negotiation response. Missing support retains framing 1 for old TS/MP4
leases. Framing is bound to a lease; DTLS reset discards leases and negotiation.

A framing 2 frame retains the existing 32-byte prefix: BE u64 sequence at 0,
camera timestamp at 8, camera connection generation at 16; slot/audio/key at
24..26. Bytes 27..30 carry a nonzero BE u32 **codec epoch**, byte 31 is `2`.
Framing 1 keeps bytes 27..31 zero. Unknown versions are rejected. NON frame
packets, DTLS protection, outer MVA1 identity and 736-byte fragmentation remain.

Connection generation changes still invalidate viewing. A codec epoch change
commits only with a complete validated IDR and resolved SPS/PPS. Repeated
parameters do not advance the epoch. Before queuing a new epoch the SDK removes
old queued video (including a partly transmitted AU), preserves queued audio and
continues monotonically increasing sequence numbers. Receiver gates dependent
video until a clean IDR after loss; initialized audio continues independently.
Older video epochs cannot reconfigure the receiver. Audio never commits an epoch.
The complete-AU queue is bounded to 16 frames / 512 KiB; single AU stays <=256 KiB.

The guarded command `live-quality SLOT LEASE16HEX QUALITY` uses the existing
request-key/task/setting operation mechanisms, plus camera SN and expected
revision. Server verifies HTTP session lease ownership; Agent requires matching
lease, framing, camera generation and sole viewer, waits for event recording
active/finishing to end, and sends command 10 at most once. Operation ID, codec
epoch, viewing lease and connection generation are distinct identities.

Browser framing is separately defined in docs/LIVE.md; it
is not a replacement for the authenticated UDP device transport.
