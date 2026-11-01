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

- **RAM cost, measured** (clean `frdm_rw612` build, this PR's tip): **403160 / 983040 bytes
  (41.01%)**, up from PR #64's tip (`eca6ca6`, ADR 0007's final state) at 371272 B (37.77%) -
  **+31888 bytes (+3.24 points)** for `QcPresetCatalog`/`QcPresetPrinter` combined
  (`QcPresetCatalog` itself: `kMaxFolders` x (`kMaxPresetsPerFolder` x a ~36-byte
  `etl::string<32>` name slot + two folder-level name fields) is the dominant share;
  `QcPresetPrinter`'s own 2048 B stack plus bookkeeping fields account for the rest).
- **Stack margins** (`-fstack-usage`, worst-case sequential call chain per thread, GCC's
  inlining folded `decodeFile()`/`printCatalog()`/`labelFor()`/`findFolder()`/`addFolder()`
  into their callers at this build's optimization level - no separate symbol, no separate
  frame to add):

  | Thread | Call chain | Budget | Depth | Margin |
  | --- | --- | --- | --- | --- |
  | `qc_dispatch` | `threadEntryTrampoline()` -> `onMessage()` (`decodeFile()` inlined) -> `addPreset()` | 2048 B | 32+352+56 = 440 B | ~78.5% free |
  | `qc_heartbeat` | `runHeartbeat()` (`logFileStreamSummary()`/`requestPrint()` inlined) -> `sendMessage()` | 2048 B | 512+200 = 712 B | ~65.2% free |
  | `qc_preset_print` | `threadEntryTrampoline()` -> `run()` (`printCatalog()` inlined) | 2048 B | 40 B | ~98% free |

  `qc_heartbeat`'s depth is essentially unchanged from ADR 0007's own measurement (720 B) -
  `requestPrint()` itself is just a pointer/bool/u32 assignment and a semaphore give, not a
  print. Every object this issue adds that's bigger than a few bytes
  (`QcPresetCatalog::folders_`, `QcPresetPrinter`'s stack/thread struct) is static/member
  storage, never a stack local - the same discipline this firmware's three prior stack
  overflows (`usbh`, `qc_heartbeat`, `qc_session`) were each fixed by applying.
- Picking `kMaxFolders`/`kMaxPresetsPerFolder` from the owner's one unit, rather than the
  protocol's fixed-256-slots-per-folder worst case, is a real bet: a unit with meaningfully
  more named content than this evidence (more than 8 folders ever holding a named preset, or
  more than 64 named presets in one folder) will see `folderOverflowCount()`/
  `presetOverflowCount()` go non-zero and the printed listing say so plainly, rather than
  silently showing everything. If that turns out to matter in practice, the fix is raising
  these two constants with new evidence, not redesigning the catalog.
- **Not verified on real hardware.** The folder/preset counts and names in Context come from
  ADR 0007's own hardware runs; this issue's own catalog/printer code has not itself been
  flashed. See issue #10's closing comment for the exact commit and expected log output to
  check that against.
