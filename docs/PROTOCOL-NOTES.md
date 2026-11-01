# Protocol findings and deviations

Findings from porting qc-mcp's protocol to the FRDM-RW612 devboard that go beyond
(or sharpen) qc-mcp's own `PROTOCOL.md`. This is a write-up, not a decision record — see
[ADR 0001](adr/0001-record-architecture-decisions.md) for that distinction.

Numbers below (sizes, timings, folder contents) come from **one unit**: a Quad Cortex
Mini on CorOS 4.1.1. Another unit, with different content or firmware, may differ —
flagged inline where it matters.

## Transport and framing

- **HID report size is 129 bytes on the wire**, not 128: 1 report-id byte + a 128-byte
  body (chunk length + flags + up to 126 bytes of payload). An early implementation used
  128 bytes total and split each real report into a 128-byte transfer plus a spurious
  1-byte trailing "report." Fixed and confirmed on hardware (#4, #7; `firmware/README.md`
  "HID report framing").
- **`SET_REPORT` (the only way to send on this device — interface 5 has no interrupt OUT
  endpoint) returns `-EPIPE` on every send, yet the data lands.** Confirmed functionally,
  not just inferred: repeated `Version` READs were reliably answered for several minutes
  while every send still returned `-EPIPE` (#7, #8). qc-mcp's own `transport.py` comment
  on the equivalent Windows code (`ERROR_GEN_FAILURE(31)`) calls this **ambiguous by
  design**: the same code comes back both when a write plainly lands on an exclusive
  handle, and when a write never reaches the device on a shared one — qc-mcp counts and
  exposes it rather than treating it as benign. Our firmware's conclusion ("non-fatal,
  continue") rests on the same kind of indirect evidence (replies kept arriving), not a
  byte-level confirmation, since Zephyr's EHCI driver has no bytes-transferred count for
  an OUT control transfer either. Treat as confirmed-for-this-device, not proven in
  general.

## Handshake and heartbeat

- **Version reply latency runs up to ~720ms**; the original 500ms wait
  (`kVersionReplyTimeout`) was too short and reliably logged `gotReply=0`, falling back to
  a hardcoded `4.1.0` string even though the device always replied, just late. Widened to
  1000ms; confirmed on hardware reporting the real `4.1.1` (#7, #8).
- **The 331/10/14-byte messages seen right after connecting are not a state burst** — an
  earlier read of the hardware log. They are all command `10`, the (delayed) `Version`
  reply itself, decoded during #8's debugging. `firmware/README.md`'s handshake section
  has been corrected to say this plainly.
- **The heartbeat now starts before the handshake, matching qc-mcp's order.** qc-mcp's own
  `transport.py::open()` does `_start_heartbeat()` then `_handshake()`, with a comment
  that the heartbeat is what "keep[s] session online (required for reads)." An earlier
  firmware version started the heartbeat after the handshake instead; the fix reordered it
  to match qc-mcp (#8). That reorder landed in the same change as the widened Version
  timeout above, so the successful hardware run doesn't isolate the ordering change by
  itself — it doesn't show that starting the heartbeat after the handshake can't also work.
- **"Reads require the heartbeat" is still only qc-mcp's documented claim, not an A/B
  result from this firmware.** Silence after the handshake proves nothing by itself unless
  a read was actually sent — our firmware's own silent run (#8, before #61) sent no reads
  of its own, so it couldn't have ruled the heartbeat in or out either way. No
  heartbeat-off comparison has been run (commenting out the `startHeartbeat()` call and
  confirming reads then fail would do it). `firmware/README.md`'s KeepAlive section flags
  this explicitly.
- **The device doesn't answer the `File` READ, and preset-change pushes don't appear,
  until the twelve post-connect state READs are sent.** Before them (`ModelRepo`,
  `ModuleStats`, `UndoRedo`, `IOSettings`, `GeneralSettings`, `Mode`, `GlobalEQ`,
  `MasterVolume`, `GlobalTempo`, `Scene`, `PresetDirty`, `SetlistPosition` — qc-mcp's own
  `transport.py::_handshake()` list, sent fire-and-forget right after `Connection`
  UPDATE), a 30s `File` READ window produced a single message total (the handshake's own
  delayed `Version` reply) and no preset-change push at all. With them in place, the same
  device answered the `File` READ and pushed a compressed `RecallPreset` on a preset
  change (#8, #60, #61).

## `File` READ (directory listing)

- **`omit_factory_content=true` is the option that shrinks the listing; `short_listing`
  has no visible effect.** An earlier hardware run read it the other way around, but that
  run had unseparated variants (`short_listing`'s reply landed late and was counted into
  the next variant's window); a cleanly-separated re-run corrected it. On this unit:
  `omit_factory_content` → 20 messages, ~30 KB (15 folders: user folders, downloads, and
  empty plugin-`User` placeholders); default → ~570 messages, ~716 KB, with single
  messages of at least 113 KB (#33).
- **`is_factory` is `0` for plugin-library presets**, so it doesn't distinguish them from
  genuine user content — the plugin presets left out by `omit_factory_content` are
  identified by path (`/opt/neuraldsp/Plugins/...`), not by that flag (#33, #66).
- **Empty preset names mark unused slots**, not a decode error — confirmed against the
  owner's own device content (e.g. the `Practice` folder's 254 empty names alongside its
  2 real presets, "Practice 1" and "Dark Jazz") (#33).
- **A downloads folder (key `cloud-0-1`) has no name.** Seen in the `omit_factory_content`
  listing alongside 25 named presets inside it (#33).
- **`File` replies can start arriving more than 1.5s after the request** — variants in the
  #33 experiment needed quiet-window and inter-variant separation (5000ms quiet window,
  10s drain gap) to avoid one variant's late reply contaminating the next's count. Final
  buffer sizes for the reassembly/inflate path are still being decided in #33 — refer
  there rather than to a specific number here.

## Message sizes observed

(All from the same unit; buffer sizing based on these is #33's job, not restated here.)

- `ModelRepo`: ~59 KB uncompressed wire size (#33).
- `ModuleStats`: ~28 KB (#33).
- `RecallPreset`: inflates to roughly 13–23 KB depending on the preset (one preset at
  ~13235 bytes, another inferred at ~22667 bytes) — the first gzip-compressed message
  confirmed decompressing correctly on real hardware (#33, #61).
- `GlobalTempo` arrives continuously, about 4 times a second, for the whole session once
  it's live (#61).

## Board-side lessons (devkit/toolchain, not protocol)

- **Zephyr's internal `usbh` thread has a small stack (1536 bytes)**, and this project
  builds with `CONFIG_LOG_MODE_IMMEDIATE=y`, so any enabled `LOG_*` call on that thread
  formats synchronously on its own stack. A diagnostic log call there caused a real
  stack-overflow crash; the fix is atomic counters instead of logging on that thread, with
  real logging done from a thread with headroom (#8). #33's work hit the same kind of
  stack-capacity exhaustion twice more, on different threads: `qc_heartbeat`'s per-variant
  logging locals were part of its permanently-reserved frame rather than a one-off cost,
  fixed by moving them into their own method and doubling `heartbeatStack_` (2048 → 4096);
  and `qc_session`'s handshake path materialized a ~7 KB `std::array` reset by value on its
  2048-byte stack, fixed by dropping the reset (bounding reads by count instead). Both were
  found via `addr2line` against the crash PC/LR rather than guessed.
- **A separate #33 crash was an out-of-bounds write, not a capacity limit.** `appendTail()`
  sized its scratch buffer for a single report's payload (126 bytes, 134 with the trailer),
  but `feed()`'s overflow-transition path could hand it the entire accumulated buffer — up
  to `kReassemblyCapacity`, 4096 bytes — in one call, writing past the 134-byte array and
  corrupting the stack. More stack headroom would not have fixed this; the fix was
  rewriting `appendTail()` to need only the trailer's 8 bytes regardless of input length.
- **Zephyr 4.5's USB host API dropped `usbh_xfer_free()`** in favor of ref-counted
  `uhc_xfer_unref()` (no `dev` argument) — the one call site this project still had was
  updated; every other USB host API used here was unaffected (#54).
- **`native_sim` doesn't build on macOS** — it uses real Linux syscalls, not a portable
  POSIX layer, so it reports "FILTERED: Native platform requires Linux" there by design.
  Local testing on macOS uses `qemu_cortex_m3` instead (`firmware/README.md`, "HID report
  framing").

## Boot ordering (#65, open)

If the board powers on before the Quad Cortex has finished booting, the device enumerates
over USB before it answers any protocol message. The handshake's `Version` READ then
times out, mirrors the `4.1.0` fallback, and still reports the session as "complete" —
nothing retries, so the session is stuck on the wrong version until the board resets.
Observed needing up to three power-cycles in one case (device took ~15.6s to enumerate on
the first). Unconfirmed: how long a late-booting unit actually takes between enumerating
and answering, and whether it needs anything beyond a repeated `Version` READ. Not fixed
yet — tracked in #65.

## To revisit once #33, #10, #11 land

- The `File`/message-size figures above are a snapshot from #33's measurement passes, not
  final sizing — #33 is still deciding real buffer sizes.
- No heartbeat-off A/B test has been run; if #10/#11 touch the session lifecycle, that gap
  is worth closing then.
- Plugin-library preset listing (#66) and the boot-ordering retry (#65) are both still
  open — this doc describes the evidence gathered, not a resolution.
