# ADR 0008: Preset catalog capacity, and a dedicated thread to print it

Date: 2026-11-01
Status: Accepted

## Context

Issue #10 is MVP1's acceptance criterion: print the parsed preset names to the serial
console, and hold them in memory (finishing issue #9). `QcFileMessage.hpp`'s `decodeFile()`
(issue #59, [ADR 0006](0006-file-message-codec.md)) already turns a `File` UPDATE's bytes
into one callback per preset slot - named or not - but its `FilePreset` fields are
`std::string_view`s into the caller's reassembly/inflate buffer, which [ADR 0007](0007-file-reassembly-buffer-sizing.md)
confirms gets reused by the very next message. Something has to copy the names out before
they're overwritten, and something has to print the result without breaking two constraints
this firmware has already paid for in hardware crashes:

- **No printing from `usbh` or the dispatcher thread.** `CONFIG_LOG_MODE_IMMEDIATE`
  (`prj.conf`) makes every `LOG_INF` call block the calling thread for the UART write. Issue
  #8's hardware debugging already crashed `usbh` (1536 B stack) from a single diagnostic log
  call; issues #8/#33/#60 established the pattern of handing real work off a worker thread
  instead.
- **Printing must never delay the ~200ms KeepAlive.** The real hardware listing (ADR 0007,
  commit 219cf6e, `omit_factory_content=true`) has 48 named presets across 3 folders - one
  `LOG_INF` line per preset plus a folder header is enough lines that doing it inline on
  `qc_heartbeat` (the thread that detects the stream's finish and sends every KeepAlive)
  would delay that thread's next send by however long the whole listing takes to print.
- **Big objects go in static/member storage, never a stack temporary.** Three separate real
  hardware stack overflows so far (`usbh`, `qc_heartbeat`, `qc_session`, most recently commit
  219cf6e's `folders_ = {}` bug) all trace back to a large object materializing on a thread's
  stack. A preset catalog sized to hold real content is not small.

Real hardware evidence for sizing (ADR 0007, commit 219cf6e, `omit_factory_content=true`):
15 folders total, only 3 of which ever hold a *named* preset (`My Presets`: 21 named/235
empty slots; `Practice`: 2 named/254 empty, matching the owner's confirmed "Practice 1" and
"Dark Jazz"; `cloud-0-1`/Downloads: 25 named) - the other 12 are empty plugin `User` folders
with zero named presets. Every folder is a fixed 256-slot table regardless of how many of
those slots are actually named.

## Decision

1. **`QcPresetCatalog.hpp`** is a plain, non-Zephyr-dependent data structure (same shape as
   `QcFileStream.hpp`/`QcMessageTally.hpp`): fixed `etl::array`/`etl::vector` storage, no heap.
   `addPreset()` is the single place issue #10's three real-hardware rules are enforced:
   - An empty-named slot is never stored, but a stored preset keeps its real `files[]`
     position (`FilePreset::index`), so gaps from skipped slots are visible in the printed
     index.
   - A folder is only ever registered on its *first* named preset - a folder that never gets
     one (an empty plugin `User` folder) simply never appears in `folders()`. This gets
     "leave out folders with no named presets" for free, with no separate empty-folder filter.
   - `cloud-0-1`'s real, empty folder name is special-cased to the label "Downloads"; any
     other folder with an empty name (not seen on real hardware, but not ruled out by the
     protocol) falls back to its key rather than a blank line.
   - Capacities: `kMaxFolders = 8` (~2.5x the observed 3 folders that ever hold a named
     preset), `kMaxPresetsPerFolder = 64` (~2.5x the observed max of 25, in Downloads),
     `kNameCapacity = 32` (double the longest observed real name, "Salaatti-Classic").
     Deliberately *not* sized for the protocol's worst-case fully-named 256-slot folder the
     way ADR 0007 sized the reassembly buffer: that sizing exists because an oversized
     message would otherwise be silently truncated mid-decode, where here running out instead
     increments a counted, reported overflow (`folderOverflowCount()`/`presetOverflowCount()`)
     - a different, cheaper way to stay safe that doesn't need the same worst-case budget.
   - `reset()` bumps an atomic `generation()` counter alongside clearing state - see point 3.
2. **`QcPresetPrinter.hpp`** owns a dedicated, low-priority (`kThreadPriority = 9`, below
   every other named thread) worker thread, started lazily the same way
   `QcDispatcher`/`QcGzipInflater` are. `QcSession::logFileStreamSummary()` (the stream-finish
   hook, `qc_heartbeat`) calls `requestPrint()` once per finished stream instead of printing
   inline; the actual `LOG_INF`/`LOG_WRN` calls happen entirely on `qc_preset_print`, so no
   amount of listing size can delay `qc_heartbeat`'s own KeepAlive send - Zephyr's preemptive
   scheduler always lets the strictly-higher-priority heartbeat thread run first, and the two
   threads share no lock that could make one wait on the other.
3. **No mutex between the dispatcher thread's writes and the printer thread's reads.**
   `QcSession::onMessage()` only calls `presetCatalog_.addPreset()` while
   `fileStream_.isActive()` is true, under `fileStreamMutex_`; `logFileStreamSummary()` only
   calls `requestPrint()` after `checkTimeout()` has already set that same flag false under
   that same lock. So by construction, nothing writes to the catalog again until the next
   `reset()` - the printer thread can read it with no lock of its own. The one remaining race
   (a reconnect's `reset()` landing while a slow print is still in progress) is caught cheaply
   by the generation counter from point 1: the printer checks it before starting and before
   each folder, and aborts rather than print a mix of two listings. This is accepted as a
   rare, cosmetic-only edge case (a short/aborted print, never a memory-safety issue, since
   `reset()` only overwrites the same fixed-size static storage) rather than adding a second
   mutex shared between `qc_heartbeat`/`qc_session` and a worker thread.

## Consequences

- **RAM cost, measured** (clean `frdm_rw612` build, this PR's tip after the review fixes
  below): **406984 / 983040 bytes (41.40%)**, up from PR #64's tip (`eca6ca6`, ADR 0007's
  final state) at 371272 B (37.77%) - **+35712 bytes (+3.63 points)** for
  `QcPresetCatalog`/`QcPresetPrinter` combined (`QcPresetCatalog` itself: `kMaxFolders` x
  (`kMaxPresetsPerFolder` x a ~36-byte `etl::string<32>` name slot + two folder-level name
  fields) is still the dominant share; `QcPresetPrinter`'s own 2048 B stack, bookkeeping
  fields, and - after the review fixes - its per-folder `FolderSnapshot` scratch buffer
  account for the rest). This is **+3824 bytes** over the 403160 B this ADR originally
  reported at commit `dfc0b4f` (before PR #68's review fixes): `Folder::keyLength`/`keyHash`
  (12 B x 8 folders = 96 B) plus `QcPresetPrinter`'s `FolderSnapshot` member
  (a `NameString` + `kMaxPresetsPerFolder` `PresetEntry`s, ~2.6 KB) and its small
  `requestMutex_`/`PendingRequest`/`catalogMutex_` additions account for the difference.
- **Stack margins** (`-fstack-usage`, worst-case sequential call chain per thread, GCC's
  inlining folded `decodeFile()`/`labelFor()`/`findFolder()`/`addFolder()`/`fnv1a64()`/
  `copyPresetEntries()` into their callers at this build's optimization level - no separate
  symbol, no separate frame to add):

  | Thread | Call chain | Budget | Depth | Margin |
  | --- | --- | --- | --- | --- |
  | `qc_dispatch` | `threadEntryTrampoline()` -> `onMessage()` (`decodeFile()` inlined) -> `addPreset()` (`findFolder()`/`addFolder()`/`fnv1a64()` inlined) | 2048 B | 32+352+40 = 424 B | ~79.3% free |
  | `qc_heartbeat` | `runHeartbeat()` (`logFileStreamSummary()`/`requestPrint()`/`isListingIncomplete()` inlined) -> `sendMessage()` | 2048 B | 512+200 = 712 B | ~65.2% free |
  | `qc_preset_print` | `threadEntryTrampoline()` (`run()` inlined) -> `printCatalog()` (`copyPresetEntries()` inlined) | 2048 B | 16+96 = 112 B | ~94.5% free |

  `qc_dispatch` is actually a few bytes *shallower* than before (424 B vs. 440 B) - the
  hash-based `findFolder()` from the review fixes below needs less local state than the old
  string comparison. `qc_heartbeat` is unchanged (712 B, matching ADR 0007's own 720 B
  measurement) - `isListingIncomplete()` is a handful of bool comparisons, inlined for free.
  `qc_preset_print` grew from one 40 B frame to two frames totalling 112 B now that
  `printCatalog()` is too large for GCC to fold into `run()` at this optimization level (the
  per-folder lock/copy/print loop from the review fixes below) - still under 6% of its 2048 B
  budget. Every object bigger than a few bytes (`QcPresetCatalog::folders_`,
  `QcPresetPrinter`'s stack/thread struct, `FolderSnapshot`) is static/member storage, never a
  stack local - the same discipline this firmware's three prior stack overflows (`usbh`,
  `qc_heartbeat`, `qc_session`) were each fixed by applying.
- Picking `kMaxFolders`/`kMaxPresetsPerFolder` from the owner's one unit, rather than the
  protocol's fixed-256-slots-per-folder worst case, is a real bet: a unit with meaningfully
  more named content than this evidence (more than 8 folders ever holding a named preset, or
  more than 64 named presets in one folder) will see `folderOverflowCount()`/
  `presetOverflowCount()` go non-zero and the printed listing say so plainly, rather than
  silently showing everything. If that turns out to matter in practice, the fix is raising
  these two constants with new evidence, not redesigning the catalog.
- **Verified on real hardware for the listing itself**: commit `dfc0b4f` printed the full
  catalog on the owner's unit - 3 folders, 48 presets, confirmed matching. **The review fixes
  below have not themselves been flashed** - they're correctness/robustness fixes to code
  paths that commit's hardware run didn't exercise (no folder/preset overflow, no 30s
  timeout, no folder key over 32 bytes on that unit) and produce byte-identical output for
  that same run (verified by inspection: the only wording change is the folder-capacity
  warning, which only fires when `folderOverflowCount() > 0`, never true on that unit). See
  issue #10's closing comment for the commit to flash and a smoke test.

## Review fixes (Copilot, PR #68)

Five issues from Copilot's review of the commit this ADR originally described, all fixed in
the same PR:

1. **`QcPresetPrinter::requestPrint()`/`run()` raced on three separate field writes.** A
   second `requestPrint()` landing between two of `pendingCatalog_`/`pendingGeneration_`/
   `pendingIncomplete_`'s assignments (the low-priority worker delayed past its
   `k_sem_take()`) could let `run()` read a mix of two requests. Fixed with a small
   `requestMutex_` (a `ScopedKMutex`, matching this file's existing lock discipline) around
   publishing all three fields as one step in `requestPrint()`, and around reading them back
   as one step (into a local `PendingRequest`) in `run()` - printing itself still happens
   outside that lock. Not practical to regression-test as an actual race (this codebase
   doesn't unit-test its other OS-thread hand-off classes - `QcDispatcher`, `QcGzipInflater` -
   for the same reason); correct by the same construction argument point 3 above already
   makes for `QcPresetCatalog`'s own quiescence.
2. **`printCatalog()` iterated the catalog's live storage while a reconnect could be
   resetting it.** The existing generation check (point 3 above) only caught a reset that
   landed *between* folders, not one landing *while* a folder's `presets` vector was being
   read - an unsynchronized read of storage another thread could be clearing/refilling,
   unsafe regardless of the storage being fixed-size. Fixed by copying one folder's data
   (name + every preset entry) out of the catalog into a member-storage `FolderSnapshot`
   while holding `catalogMutex_` - the *same* mutex `QcSession` holds for every catalog write
   (`fileStreamMutex_`, wired in via `setCatalogMutex()`) - then releasing the lock before the
   slow `LOG_INF`/`LOG_WRN` calls that print it. Each lock span is a small, bounded copy,
   never the printing itself, so this can't delay `qc_heartbeat`'s own brief use of the same
   mutex. The copy step (`detail::copyPresetEntries()`) is a free function with no locking of
   its own, specifically so its only real failure mode (writing past the snapshot's bound) is
   unit-testable without a kernel fixture - see `tests/preset_printer`.
3. **`QcPresetCatalog::findFolder()` compared a bounded key against a full one.**
   `addFolder()` truncates the stored `key` to `kNameCapacity` (32 B) for display, but
   `findFolder()` compared that truncated copy against each new preset's *full* incoming key
   - a key longer than 32 B (real plugin "User" folder keys already run past that) never
   matched on a second preset, so every one of them registered its own new folder and could
   exhaust `kMaxFolders` after as few as `kMaxFolders + 1` presets under one long key. Fixed
   by giving `Folder` a stable identity independent of the bounded display copy: a
   (length, FNV-1a 64-bit hash) pair computed from the *full* key, stored as
   `keyLength`/`keyHash` and used by `findFolder()` instead of the truncated string compare.
   `key` stays for display/debugging only. Regression-tested in `tests/preset_catalog`: many
   presets under one >32 B key land in one folder, two distinct >32 B keys sharing the same
   32 B prefix still register as two folders, and a folder's own display *name* (not just its
   key) still truncates safely rather than overflowing.
4. **The "folder(s) dropped" warning counted presets, not folders.** `folderOverflowCount()`
   increments once per *preset* rejected because no folder slot was free, not once per
   distinct folder that couldn't be added - a ninth folder with ten named presets would have
   logged "10 folder(s) dropped" instead of "1". Fixed by rewording the log line to "N
   preset(s) rejected - folder capacity (N) reached"; the counter's own semantics (and name)
   are unchanged; the file-level comments already described it in terms of overflow rather
   than distinct folders.
5. **A stream cut off by the 30s overall timeout wasn't flagged as potentially incomplete.**
   `checkTimeout()` deactivates the stream the instant `kOverallTimeoutMs` is hit *even if the
   device is still sending* (`onMessage()` gates every catalog write on `isActive()`), so a
   preset still in flight at that instant is silently never added - with nothing else counting
   it as dropped. The code this ADR originally described left `summary.timedOut` out of the
   incompleteness check on the (incomplete) reasoning that a timeout alone wasn't evidence of
   loss - true for the no-messages-at-all case, wrong for this one. Fixed by folding
   `timedOut` into the same check, pulled out as `QcFileStream::isListingIncomplete()` (a
   free function taking the `Summary` and a `bool catalogOverflowed`, no `QcSession`/USB
   dependency) so it's directly unit-testable - see the `isListingIncomplete` tests added to
   `tests/dispatch_messages` (which needed `qemu_cortex_m3` added to its `platform_allow`
   alongside `native_sim`, matching `tests/preset_catalog`'s own list).
