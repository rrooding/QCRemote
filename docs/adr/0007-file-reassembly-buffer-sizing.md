# ADR 0007: File reassembly/inflate buffer sizing, and dropping ModelRepo/ModuleStats from dispatch

Date: 2026-11-01
Status: Accepted

## Context

Issue #33 asked for real sizes for the placeholders `kReassemblyCapacity` (`QcHidBridge.hpp`),
the dispatcher mailbox (`QcDispatcher.hpp`), and `kMaxCompressedInputSize`/`kMaxInflatedSize`
(`QcGzip.hpp`), all originally 4096/4096/4096/16384 and picked with no real traffic to measure
against. A series of hardware runs (see `firmware/README.md`'s now-condensed issue #33 section,
and the issue's own comment history) measured real `File`-listing traffic at each of those
limits before this ADR's decision:

- **Default `FileMessage` READ** (qc-mcp's own shape: no `short_listing`/`omit_factory_content`
  set): ~550-570 messages, ~716 KB wire, largest single message ~113 KB (a factory/plugin
  folder). 29-32 of those overflowed the original 4096-byte capacity.
- **`short_listing=true`**: measured no effect at all once variants were cleanly separated
  (an earlier, contaminated run had pointed at this option; a clean re-run corrected that).
- **`omit_factory_content=true`**: shrinks the listing to 20 messages, ~30 KB wire, largest
  10193 bytes, zero overflows - 15 preset folders (`My Presets`: 21 named/235 empty slots;
  `Practice`: 2 named/254 empty, matching the owner's unit; `cloud-0-1`: 25 named; 12 empty
  `User` plugin folders), with every factory/plugin-library preset left out (confirmed still
  present on the device, just not listed). The owner confirmed `My Presets` really does hold
  21 presets, matching this listing exactly.
- A compressed `RecallPreset` push inflated to 22667 bytes for one preset on the owner's unit
  (inferred to be "Practice 1"; a second preset, "Dark Jazz", measured 13235 bytes) - both past
  the original 16384-byte `kMaxInflatedSize`.
- `ModelRepo` and `ModuleStats` (two of the twelve post-connect state READs, issue #61) each
  overflowed the original 4096-byte reassembly capacity once, at 59219 and 28354 bytes
  respectively, in the default (non-`omit_factory_content`) run.

The owner's decision, once `omit_factory_content=true` was confirmed to solve the listing-size
problem: use it for every `File` READ, remove the measurement-only variant experiment
(`QcFileReadExperiment.hpp`, the per-folder tables, `CONFIG_THREAD_ANALYZER`, and the
per-command overflow/size tallies that existed only to gather this evidence), and pick final
sizes.

### Sizing `kReassemblyCapacity` for a worst-case folder, not today's 10193 bytes

`My Presets`'s 10193-byte message is mostly empty slots (21 named, 235 empty - every preset
folder on this protocol is a fixed 256-slot table, same shape confirmed for `Practice` and
documented by qc-mcp's `directory.py` for `SetlistPosition`'s own table). A folder with more
named presets costs far more per slot than an empty one: checked against qc-mcp's own
descriptor set (`qc_descriptors-4.1.pb`), each slot is a `ProductData` message -

```
ProductData
   1  key                    string
   2  index                  int32
   3  name                   string
   4  author                 string
   5  author_id              string
   6  coros_version          string
   7  tags                   repeated string
   8  date                   string
   9  gain                   int32
   10 cloud_id               string
   11 description            string
   12 shared_by              string
   13 factory_dependencies   repeated ProductDependency
   14 product_dependencies   repeated ProductDependency
   15 instrument             int32
   16 device                 int32
   17 extraMetadata          bytes
   18 old_key                string
   19 is_readonly            bool
   20 version                string
   21 date_ms_since_epoch    int64
```

An empty slot plausibly encodes little more than its position (a tag/length pair plus a small
`index` varint - a handful of bytes); a named one can carry a dozen-plus of the 21 fields above
(name, author, dates, cloud/version ids, tags, dependency lists, ...) - metadata in units proto3
doesn't bound (strings, repeated fields, a raw `bytes` blob), not just a longer name string.
Backing out an empty slot's small cost from the real 10193-byte/256-slot message puts each named
slot at roughly 410-460 bytes there - and the one real data point for a *heavily*-named folder
(the default listing's 113 KB message, from a factory/plugin folder with far more named entries
than 21) is consistent with a similar per-entry cost scaled up, not something wildly larger.
256 named slots (every slot full - the realistic worst case for a user who fills a folder) at
~511 bytes/slot is 131072 bytes (128 KiB) - comfortably above the ~410-460 byte/slot the real
evidence implies, without assuming an implausible blow-up in per-slot cost.

## Decision

1. **`FileMessage` READ always sends `omit_factory_content=true`** (`QcFileRequest.hpp`,
   already landed). `short_listing` is no longer sent at all (measured no effect). The entire
   variant-measurement scaffold is removed: `QcFileReadExperiment.hpp`, its per-folder table,
   its call sites in `QcSession.hpp`, `CONFIG_THREAD_ANALYZER` (`prj.conf`), the per-command
   overflow tally (`QcReassemblyOverflowTally.hpp`) and the trailer-retention machinery in
   `HidReassembler.hpp` that only existed to attribute an overflow to a command
   (`overflowTrailer()`/`appendTail()`, and `QcMessage.hpp`'s `decodeTrailerCommand()`) - none
   of it has a reason to exist once real sizes are picked from the evidence it produced.
2. **`kReassemblyCapacity` (`QcHidBridge.hpp`) and the dispatcher mailbox (`QcDispatcher.hpp`,
   kept mirrored by hand, same as before) are both 131072 bytes (128 KiB)** - sized for a
   worst-case, fully-named 256-preset user folder per the Context section above, not today's
   mostly-empty 10193-byte message.
3. **`kMaxInflatedSize` (`QcGzip.hpp`) is 32768 bytes (32 KiB)** - ~44% headroom above the
   22667-byte `RecallPreset` the owner's unit actually produced, without just doubling blindly.
   **`kMaxCompressedInputSize` is 8192 bytes** - kept at the same 4x ratio to
   `kMaxInflatedSize` this module has always used (headroom for this protocol's typical 3-6x
   gzip ratios).
4. **`QcFileStream`'s quiet window is permanently 5000ms** (was 1500ms). Evidence: a hardware
   run sending unseparated variants back-to-back found a genuine pause *inside* a single
   reply - more than 1500ms, but still part of the same reply, not the reply ending - got read
   as the stream finishing, so the real tail that followed was miscounted as a separate
   request's traffic. 5000ms was used as the measurement window for every subsequent hardware
   run with no recurrence of that problem, which is the only real evidence available for
   picking a value - so it's kept as the permanent one rather than reverting to the
   unvalidated 1500ms.
5. **`ModelRepo` and `ModuleStats` replies are ignored outright in
   `QcSession::handleMessage()`**, before the gzip-magic check or either worker's mailbox - an
   atomic counter bump only, still usbh-safe. The twelve post-connect state READs (issue #61)
   are still all sent (`runHandshake()`, unchanged) - the device needs them sent, per the
   owner's own framing of this issue - but the preset listing doesn't need either reply, and
   both are large enough (59219 and 28354 bytes, measured) that buffering them through the
   dispatcher/gzip-inflater mailboxes would cost real RAM and contend for the gzip inflater's
   single in-flight slot for no benefit. This is also the most plausible explanation for a
   real hardware run's `gzip dropped=3` during a (pre-`omit_factory_content`) full catalog
   listing, with 0 File messages ever confirmed compressed: `ModelRepo` is the only state-READ
   command this protocol is known to sometimes gzip-compress (ADR 0005), so a late reply to it
   straggling into the listing's window is the leading candidate for occupying the gzip
   inflater's one slot - though QcGzipInflater::submit()'s drop path discards the mailbox
   before recording which command it was, so this can't be confirmed from firmware logs alone.
   It's moot going forward either way, now that neither reply ever reaches that mailbox.
6. **The per-command/per-stream measurement diagnostics are gone**; what remains is a
   concise, two-line File-stream-finish summary (counts, wire/inflated bytes and peaks,
   elapsed time, dropped/overflow/malformed counts) plus a standing `checkTrafficHealth()`
   check, run every heartbeat tick rather than only at the File stream's finish, that logs
   whenever any dropped/overflow/malformed lifetime counter changes - so an oversized message
   arriving *after* the listing (e.g. a big preset push) is still visible, not just the known
   gap during the listing itself. This also resolves a PR review comment on the removed
   per-command tally (`QcReassemblyOverflowTally`): that class's multi-field slots could be
   observed mid-update by a concurrent reader; the replacement is a handful of independent,
   single-field atomics, each read on its own, so there is nothing for a reader to catch
   half-written.

## Consequences

- **RAM cost, measured** (clean `frdm_rw612` build, this PR's tip): **371272 / 983040 bytes
  (37.77%)**, up from the pre-#33 baseline of 97816 B (9.95%) - the two 128 KiB buffers above
  account for almost all of the increase (4096 B baseline each -> 131072 B each, +253952 B
  combined), plus `kMaxInflatedSize`/`kMaxCompressedInputSize` going from 16384+4096 to
  32768+8192 (+20480 B). This is a large, deliberate fraction of the board's 960 KB RAM for one
  feature - worth flagging explicitly against issue #34's note that a smaller board is a
  possible later move. If that move happens, this sizing (and probably the fixed-buffer
  approach entirely, in favor of streaming/parsing `File` as it arrives - already noted as a
  follow-up by the owner) will need revisiting; it is not re-derived here.
- Stack margins (`-fstack-usage`, worst-case sequential call chain per thread - re-measured
  against this PR's own build, not carried forward from an earlier commit's numbers):

  | Thread | Call chain | Budget | Depth | Margin |
  | --- | --- | --- | --- | --- |
  | `usbh` | `onReportIn()` -> `handleMessage()` | 1536 B | 96 B | ~94% free |
  | `qc_session` | `run()` -> `runHandshake()` -> `sendMessage()` | 2048 B | 648 B | ~68% free |
  | `qc_heartbeat` | `runHeartbeat()` -> `sendMessage()` (worst sibling call) | 2048 B | 720 B | ~65% free |
  | `qc_dispatch` | `run()` -> `onMessage()` | 2048 B | 400 B | ~80% free |
  | `qc_gzip_inflate` | `run()` -> `onMessage()` (worst sibling call) | 4096 B | 496 B | ~88% free |

  `heartbeatStack_` is reverted to its pre-#33 2048 B (was doubled to 4096 B only for the now-
  removed variant experiment's extra locals) - the measured 720 B depth confirms that's safe.
  Every buffer raised by this ADR (`kReassemblyCapacity`, the dispatcher mailbox,
  `kMaxInflatedSize`, `kMaxCompressedInputSize`) is a class member (`HidReassembler`'s
  `buffer_`, `QcDispatcher`'s `mailbox_`, `QcGzipInflater`'s `mailbox_`/`output_`) - one static
  instance each, never a stack local, consistent with this issue's three earlier hardware
  stack overflows all being caused by large *stack* temporaries, not static storage.
- `kReassemblyCapacity`'s 128 KiB is sized for a *user* folder; it is not sized to hold the
  113 KB factory/plugin-folder message seen in the default (non-`omit_factory_content`)
  listing - that message type is no longer requested at all, so it's out of scope for this
  sizing, not an oversight.
- **Not verified on real hardware as a whole.** The 97816 B/101816 B/... figures and every
  per-command size quoted in Context came from real hardware runs on earlier commits of this
  PR; this ADR's own final sizes, the ModelRepo/ModuleStats ignore change, and the 5000ms quiet
  window as a *permanent* (not experiment-only) value have not themselves had a hardware run
  against this exact build. See `firmware/README.md`'s updated issue #33 section for the smoke
  test the next hardware run should perform.
