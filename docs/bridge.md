# Game bridge

The installed Lua actor reads the current chart and completed stage through XSanity's Lua API. It publishes a small numeric mailbox held by its update closure. The companion discovers and reads that mailbox using Windows process-query and read permissions. It does not enable logging, inject code, write process memory, follow game object pointers, or read score-history files.

## Lifetime and components

Setup configures two constants in the installed actor and records the fingerprint of that exact source. Overlay-only mode never reads completed stage statistics. Sync-only mode publishes no current-chart payload. Changing components requires XSanity closed so the running hook cannot keep the previous capture policy.

The mailbox lives only while the game runs. A completed result is repeated for up to 60 seconds so the companion can discover or reconnect, then cleared. While the companion is running, it promptly persists eligible results in its existing upload queue, including during network outages. Run the companion while playing; this bridge does not recover missed sessions from game history.

## Protocol version 1

The actor preallocates exactly 1,024 numeric Lua array entries and never resizes the array. Six bytes fit exactly in each double's integer precision. No remote string or table pointers are dereferenced.

| Lua index | Meaning |
| --- | --- |
| 1–4 | Signature: `204081632653`, `918273645546`, `672345891234`, `135792468013` |
| 5 | Protocol version, `1` |
| 6 | Slot count, `1024` |
| 7 | Sequence: odd while writing, even when published |
| 8 | UTF-8 payload length, at most 6,090 bytes |
| 9 | `GetTimeSinceStart()` at publication |
| 10–1024 | JSON bytes, six bytes per integer, least-significant byte first; unused slots cleared |

JSON contains the existing single-player `current` state, a `players` array of live side/profile names, and optionally a `results` array (at most two) plus its `completed` game-clock time. Each result includes its side and the profile name captured at the start of that attempt. Profile changes invalidate the attempt; duplicate names or profile GUIDs cannot produce account uploads. The companion matches names case-insensitively without fuzzy matching and stores the chosen account index with each pending result. The reader also accepts the previous singular result envelope during upgrades; a result without an explicitly matched profile cannot enter a new queue. The native side estimates completion time from the observed heartbeat and elapsed game time. It applies the existing capture cutoff, validation, deduplication, and website-PB comparison before upload.

## Result feedback

While syncing, the companion atomically replaces `Save/PiuCompanion/status.txt` only when an outcome changes. The data-only format starts with `PIUCOMPANION 1` followed by up to two `event-id<TAB>status-code` lines. It contains no keys, profile names, or score payloads. Entries expire after two minutes. Setup clears the file on hook replacement, relocation, component removal, or uninstall along with the other managed transient files.

The system overlay checks the 512-byte size limit and reads feedback through the same `lua.ReadFile` API used by XSanity's theme loader. The game's `RageFile` API is restricted to song folders; `FlushDirCache` and `RageFile:GetFileSize` are not exposed. It polls twice per second on `ScreenEvaluation`, and retries an unavailable reader every two seconds at menus. The mailbox's `feedback` field reports reader readiness for diagnosis without enabling logs. It displays fixed local messages for known codes and exact attempt IDs; it never evaluates file contents. Missing feedback gets a neutral unavailable message after eight seconds. Read, write, or display failures cannot block capture or uploads. Overlay-only mode creates neither result cards nor feedback files.

Acceptance is reported after the upload receipt is saved. Offline reads, credential errors, rejected uploads, and uncertain POST responses remain distinct; notifications do not change queue or retry policy. Tests exercise the native upload path, actual feedback file, and shipped Lua notification logic across processes. Live in-game rendering still requires acceptance testing.

## Plates and play eligibility

The result's optional `plate` is derived from the same tap and hold-checkpoint judgements used by xsanity's evaluation screen. It survives offline queuing and is sent as `award` in [PIU Scores' observed-play API](https://piuscores.arroweclip.se/swagger/index.html). Failed stages omit it. Scores are still submitted only when they improve the website PB; old accepted plays are not reconstructed or resubmitted.

While syncing, each gameplay update checks human control, normal judgement, 1× music rate, supported mode, and note-count modifiers at both Song and Current modifier levels. A rejected state stays rejected for that attempt, even if settings are restored. Evaluation also checks the stage's disqualification and autoplay flags. API errors during capture invalidate the attempt. These are ordinary accidental-setting checks, not anti-cheat or verification of chart-file integrity. Overlay-only mode does not inspect result statistics.

## Reader safeguards

Discovery checks only processes whose executable is the selected folder's `Program64/XSanity.exe` or `Program32/XSanity.exe`. It searches committed, readable private memory for the mailbox signature, retaining only a matching mailbox. Discovery is bounded to 16 MiB and roughly 20 ms per poll; once connected, it reads only the fixed 16 KiB block twice per poll.

The reader validates version, capacity, numeric type tags, finite values, integral lengths, and sequence. Two reads must agree on every published value. A newly found array must produce an advancing clock and sequence before it is trusted; an abandoned heap allocation cannot act as a heartbeat. Game exit, a changed folder, or a stalled mailbox triggers reconnection. Invalid snapshots never produce uploads.

This protocol currently supports the numeric Lua 5.1 array layout used by the inspected XSanity build: a 16-byte TValue containing an 8-byte double and numeric type tag `3`. This is a runtime-layout dependency, not an executable-address dependency. A game update that changes that layout must be verified before claiming compatibility; discovery rejects incompatible layouts. No memory addresses or offsets from a particular game executable are shipped.

Tests cover malformed and changing snapshots, exact capacity, UTF-8, component isolation, and the shipped actor running in a separate real Lua process through garbage collection. A live XSanity session also verified completed-result capture and an authenticated PB upload, followed by a read-only website check, with logging disabled. Verify this acceptance path again when changing the game runtime or export protocol.
