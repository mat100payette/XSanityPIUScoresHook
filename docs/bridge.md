# Game bridge

The bridge carries current-chart information and completed results from XSanity to the companion. **PIU Scores remains the source of truth for PBs.**

The installed Lua hook publishes a small in-memory mailbox. The companion reads it using Windows process-query and read permissions; logging is not required. It never writes game memory, follows game-object pointers, or reads score history.

## From play to upload

1. **Start a song.** The hook captures each player's chart, side, and in-game profile.
2. **Finish the stage.** It publishes up to two results, including eligibility and plate information.
3. **Route and compare.** The companion matches each profile to a configured account, checks the result, and compares it with the website PB.
4. **Upload an improvement.** Eligible results enter the persistent upload queue, which supports offline retry. Results that are uploaded, covered by a website PB, or skipped leave no retained score payload; receipts prevent duplicates.

A completed result stays in the mailbox for up to **60 seconds**, then clears. Run the companion while playing: it cannot recover missed sessions from game history.

### Component choices

| Installed components | Hook behavior |
| --- | --- |
| Score sync only | Captures results; publishes no current-chart payload for OBS |
| OBS overlay only | Publishes the current chart; reads no completed-stage statistics and creates no result notifications |
| Both | Enables both paths independently |

Setup writes the component choices into two hook constants and records the resulting source fingerprint. XSanity must be closed when changing components so it cannot keep running the previous hook.

## Which plays can sync?

During gameplay, the hook checks human control, supported game mode, **1× music rate**, normal judgement, and note-changing modifiers. It checks both Song and Current modifier levels.

- **Allowed:** normal judgement, including when implicit in the modifier string; BGA and scroll settings.
- **Rejected:** altered judgement, changed music rate, note-changing modifiers, unsupported modes, or nonhuman control.
- **Checked again at results:** the stage's disqualification and autoplay flags.

The first rejection stays attached to the attempt as `skip_reason`, even if settings are restored. Known reasons produce specific notifications and receipt codes. Unknown reasons or capture errors prevent upload. These checks catch accidental settings; they do not verify chart-file integrity or provide anti-cheat protection.

**XSanity API detail:** `GetAutoPlay()` returns numeric **0/1** in the inspected build. The hook validates that value and compares it explicitly because Lua treats zero as true. `IsDisqualified()` and `GetFailedAux()` return booleans.

### Player accounts

The companion matches in-game profile names case-insensitively, without fuzzy matching. It saves the selected account index with each queued result.

Changing profiles during a song invalidates the attempt. Duplicate profile names or GUIDs prevent account uploads, and a result without an explicitly matched profile cannot enter the queue.

### Plates

The optional `plate` uses the same tap and hold-checkpoint judgements as XSanity's evaluation screen. It survives offline queuing and becomes `award` in the [PIU Scores observed-play API](https://piuscores.arroweclip.se/swagger/index.html). Failed stages omit it. Previously accepted plays are not reconstructed or resubmitted.

## PB screenshots

Screenshots are independent of OBS and disabled by default. The companion uses Windows Graphics Capture for the selected game's window and crops to the client area. It never falls back to capturing the desktop.

The hook publishes the current `screen` alongside its result IDs. After three seconds on evaluation, the companion captures one provisional PNG for the eligible players on that screen. Website PB comparison decides whether to keep it; upload completion is not required. A failed PB qualifies only for an account with **Include failed PBs** enabled.

Provisional images and their metadata live in `pending-shots` under the companion's state directory. They survive offline retries, then disappear when the image is kept or every candidate is ruled out. Turning screenshots off discards provisional captures. Removing sync also cleans them up; saved screenshots remain. Capture and output-folder failures do not block uploads.

## In-game notifications

The companion returns outcomes through `Save/PiuCompanion/status.txt`. It replaces the file atomically when its contents change:

```text
PIUCOMPANION 1
<event-id><TAB><status-code>
```

There are at most two entries, with no keys, profile names, or score payloads. Entries expire after two minutes. Setup clears this file with the other managed transient files when replacing or relocating the hook, removing components, or uninstalling.

The hook reads fixed status codes for the matching attempt; it never executes file contents. A successful upload is announced only after its receipt is saved. Offline retries, credential errors, rejected uploads, and uncertain responses have distinct messages.

| Behavior | Timing |
| --- | --- |
| Poll on `ScreenEvaluation` | Twice per second |
| Retry an unavailable reader at menus | Every two seconds |
| Show unavailable status if no response arrives | After eight seconds |
| Fade a final outcome beneath the usercard | After five seconds; repeated feedback does not restart the timer |

Feedback failures cannot block capture or uploads, and notifications do not change retry policy.

**File-reader detail:** the hook enforces a 512-byte limit and uses `lua.ReadFile`, the same reader as XSanity's theme loader. `RageFile` is restricted to song folders; `FlushDirCache` and `RageFile:GetFileSize` are not exposed to Lua.

## Mailbox format — version 1

The hook allocates **1,024 numeric Lua array entries** and never resizes them. Each double holds six bytes exactly, so the reader needs no remote string or table pointers.

| Lua index | Meaning |
| --- | --- |
| 1–4 | Signature: `204081632653`, `918273645546`, `672345891234`, `135792468013` |
| 5 | Protocol version: `1` |
| 6 | Slot count: `1024` |
| 7 | Sequence: odd while writing, even when published |
| 8 | UTF-8 payload length, at most 6,090 bytes |
| 9 | `GetTimeSinceStart()` at publication |
| 10–1024 | JSON bytes, six per integer, least-significant byte first; unused slots cleared |

### JSON payload

| Field | Contents |
| --- | --- |
| `current` | Existing single-player current-chart state |
| `players` | Live sides, profile names, and current settings rejection reasons |
| `results` | Optional array of up to two completed results, each with its original side and profile |
| `completed` | Game-clock time of completion |
| `feedback` | Status-reader readiness, available without logging |
| `screen` | Current screen name, used to limit screenshots to evaluation |

The reader also accepts the previous singular `result` envelope during upgrades. It estimates completion time from the observed heartbeat and elapsed game time, then applies the capture cutoff, validation, deduplication, and website-PB comparison.

## Reading safely

Discovery is limited to the selected folder's `Program64/XSanity.exe` or `Program32/XSanity.exe`.

- **Discovery:** searches committed, readable private memory for the mailbox signature, bounded to 16 MiB and roughly 20 ms per poll.
- **Connected reads:** reads only the fixed 16 KiB mailbox block, twice per poll. Both snapshots must agree on every published value.
- **Validation:** checks version, capacity, numeric type tags, finite values, integral lengths, and sequence. A newly discovered mailbox must show an advancing clock and sequence before it is trusted.
- **Reconnection:** starts after game exit, a changed folder, or a stalled mailbox. Invalid snapshots never produce uploads.

### Runtime compatibility

The supported Lua 5.1 numeric array layout uses a 16-byte `TValue`: an 8-byte double with numeric type tag `3`. No executable-specific addresses or offsets are shipped.

An XSanity update that changes this runtime layout needs compatibility verification. Discovery rejects incompatible layouts.

## Testing and live verification

Automated checks cover malformed and changing snapshots, capacity limits, UTF-8, component isolation, and the shipped hook running in a separate Lua process through garbage collection. The cross-process tests also exercise upload outcomes, the feedback file, and notification text.

A live session has verified capture, an authenticated PB upload, and a subsequent website read with logging disabled. That baseline does not verify every later change: repeat live capture/upload checks after runtime or protocol changes, and check notification rendering in the game.
