# Firmware (NXP FRDM-RW612)

Target board: `frdm_rw612` (see [ADR 0002](../docs/adr/0002-devkit-selection.md) for why).
Zephyr, C++23 (see [CODING_STANDARDS.md](../CODING_STANDARDS.md)).

## Layout

```
firmware/
└── app/            # the actual Zephyr application (out-of-tree, "freestanding")
    ├── CMakeLists.txt
    ├── prj.conf
    ├── boards/frdm_rw612.overlay   # switches the RW612's USB peripheral to host mode
    ├── src/
    │   ├── Main.cpp
    │   ├── usbh_shim.c/.h          # plain-C boundary around Zephyr's non-C++-safe USB host API
    │   ├── UsbHostClass.hpp        # generic C++ base built on the shim
    │   ├── QcHidBridge.hpp         # claims the QC Mini's HID interface, on top of UsbHostClass
    │   ├── HidReassembler.hpp      # qc-mcp framing: reports in, one logical message out
    │   ├── HidChunker.hpp          # qc-mcp framing: a message in, reports out
    │   ├── QcGzip.hpp              # issue #6: gzip detection + inflate (ADR 0005)
    │   └── QcGzipInflater.hpp      # issue #6: runs QcGzip.hpp off the usbh thread
    └── tests/
        ├── hid_framing/            # Ztest suite for HidReassembler/HidChunker - no hardware needed
        └── gzip_inflate/           # Ztest suite for QcGzip.hpp, with real gzip fixtures
```

This repo does **not** vendor its own `west.yml`/manifest. `firmware/app` is a standard
out-of-tree Zephyr application that builds against a Zephyr workspace you set up separately
(the usual pattern for a monorepo that also holds non-firmware code, like `CODING_STANDARDS.md`
and the future `app/` JUCE client — a nested west manifest here would otherwise turn the
whole `west update` into siblings of this repo's *parent* directory, not a subfolder of it).
If that becomes annoying, revisit — this is a convention, not an ADR-level decision.

## One-time setup

Follow Zephyr's own [Getting Started Guide](https://docs.zephyrproject.org/latest/develop/getting_started/index.html)
to install the toolchain and SDK, then create a workspace pinned to the release this project
targets (v4.5.0, current stable as of writing — confirmed `frdm_rw612` and `CONFIG_STD_CPP23`
both exist in this release):

```bash
python3 -m venv ~/.venv/zephyr
source ~/.venv/zephyr/bin/activate
pip install west

west init -m https://github.com/zephyrproject-rtos/zephyr --mr v4.5.0 ~/zephyrproject
cd ~/zephyrproject
west update
west packages pip --install
west sdk install
```

BLE (for MVP2) needs NXP's binary blobs — fetch them now so they're not a surprise later:

```bash
west blobs fetch hal_nxp
```

## Build

```bash
west build -b frdm_rw612 /path/to/QCRemote/firmware/app --pristine
```

Drop `--pristine` on subsequent builds once the build directory exists.

## Flash

The FRDM-RW612's onboard **MCU-Link** probe, at least on the unit this was verified against,
enumerates as **SEGGER J-Link** (`system_profiler`/`ioreg` show `USB Vendor Name: SEGGER`,
`USB Product Name: J_Link`) — not CMSIS-DAP. So the `jlink` runner `west flash` tries by
default is the correct one; you just need SEGGER's tools installed.

Install the **J-Link Software and Documentation Pack** for your OS from
[segger.com/downloads/jlink](https://www.segger.com/downloads/jlink/) (no account needed),
then confirm `JLinkExe` is on `PATH` (SEGGER's installer typically symlinks it into
`/usr/local/bin` automatically):

```bash
which JLinkExe
```

Then just:

```bash
west flash
```

If your board's probe turns out to be running CMSIS-DAP firmware instead (NXP ships MCU-Link
boards both ways depending on batch/board revision — verify with `system_profiler
SPUSBDataTree` or `ioreg -p IOUSB -l | grep -i "USB Vendor Name\|USB Product Name"` before
assuming either way), use `west flash --runner linkserver` instead, which needs NXP's
**LinkServer** utility installed and on `PATH` (via the
[MCUXpresso Installer](https://www.nxp.com/mcuxpresso/installer) or the standalone
[LinkServer installer](https://www.nxp.com/design/design-center/software/development-software/mcuxpresso-software-and-tools-/linkserver-for-microcontrollers:LINKERSERVER)).

## Verify

Open a serial monitor on the probe's USB-CDC port (`zephyr,console` is UART, `flexcomm3`,
bridged over USB by the J-Link probe) — either `screen /dev/tty.usbmodemNNNN 115200` or
[`tio`](https://github.com/tio/tio) (`tio /dev/tty.usbmodemNNNN`), whichever's installed.
There are two `usbmodem` devices from the probe; the console is the first one enumerated.

**Verified 2026-09-15** on real hardware:

```
*** Booting Zephyr OS build v4.4.2 ***
[00:00:00.016,557] <inf> main: QC Bridge firmware skeleton up (C++202302)
```

and the onboard green LED blinks at ~1Hz. Confirms the toolchain, board target, and C++23
configuration all work end-to-end before any real protocol code lands (issues
[#3](https://github.com/rrooding/QCRemote/issues/3) onward).

## USB host bring-up (issue #3)

`boards/frdm_rw612.overlay` switches the RW612's single USB-OTG peripheral from device mode
(the board default) to host mode. `prj.conf` enables `CONFIG_USB_HOST_STACK` (Zephyr's
`[EXPERIMENTAL]` USB host stack — see [ADR 0002](../docs/adr/0002-devkit-selection.md) for
the accepted risk); the NXP EHCI controller driver auto-selects itself once the devicetree
node is enabled.

**Zephyr's public USB host headers don't compile as C++** (confirmed against a real build,
Zephyr v4.4.2): `zephyr/usb/usbh.h` has a struct field literally named `class`, and
`zephyr/drivers/usb/uhc.h` relies on C's implicit `void*` conversions in several inline
functions. Both are genuine bugs in Zephyr's public API, not something fixable from the C++
side — worth reporting upstream.

The fix is [`usbh_shim.c`](app/src/usbh_shim.c)/[`usbh_shim.h`](app/src/usbh_shim.h): a plain
C file (compiled under C, where neither issue applies) exposing a minimal, C++-safe API —
register a VID/PID filter and three callbacks, get the USB host controller started. It also
depends on `subsys/usb/host`'s internal headers (`usbh_class.h`, `usbh_desc.h`), which aren't
part of Zephyr's public API but which every USB host class driver needs, including Zephyr's
own in-tree ones (MSC, UAC2, UVC) — `CMakeLists.txt` adds `${ZEPHYR_BASE}/subsys/usb/host` as
an include path to reach them.

[`UsbHostClass.hpp`](app/src/UsbHostClass.hpp) wraps that shim in a small, generic C++ base
class (`OnInit()`/`OnProbe()`/`OnRemoved()` virtuals) so nothing outside `usbh_shim.c` ever
includes a Zephyr USB header. `QcHidBridge` ([src/QcHidBridge.hpp](app/src/QcHidBridge.hpp))
subclasses it: filters for the Quad Cortex Mini's VID/PID (`0x152A`/`0x892F`, per
[qc-mcp](https://github.com/lexasoft123/qc-mcp)) and claims only interface 5 (the vendor HID
control interface), rejecting every other interface the composite device exposes. `Main.cpp`
just owns a `QcHidBridge` and calls `Start()` — no USB host plumbing visible there at all.

No transfers are submitted yet — `shim_completion_cb` in `usbh_shim.c` is a stub returning
`-ENOTSUP` — that lands with the HID framing and session-handshake work (issues #4-#8).

**Verified 2026-09-15** against a real Quad Cortex Mini:

```
*** Booting Zephyr OS build v4.4.2 ***
<inf> main: QC Bridge firmware skeleton up (C++202302)
<inf> main: USB host enabled, waiting for Quad Cortex Mini
<inf> usbh_dev: New device with address 1 state 2
<inf> usbh_dev: Configuration 1 bNumInterfaces 6
<inf> main: Claimed Quad Cortex Mini HID interface 5
```

The device enumerates as a composite audio+HID device (6 interfaces), consistent with
qc-mcp's findings, and interface 5 is correctly claimed.

One thing learned along the way, now baked into the design: with a `NULL`/VID-PID-only
class filter, Zephyr's USB host stack calls `probe()` **once per device**, not once per
interface — it hands back `USBH_CLASS_IFNUM_DEVICE` (255), a sentinel, not a real interface
number. `usbh_shim.c` looks up the target interface (5) itself via `usbh_desc_get_iface()`
rather than trusting the `iface` argument `probe()` receives; `qc_usbh_filter` carries that
target interface number alongside vid/pid.

## HID report framing (issue #4, part 1 of 2)

[`HidReassembler.hpp`](app/src/HidReassembler.hpp) (reports in, one logical message out) and
[`HidChunker.hpp`](app/src/HidChunker.hpp) (a message in, reports out) implement qc-mcp's
129-byte HID report framing (1 report-id byte + a 128-byte body: chunk length + flags +
up to 126 bytes of payload — see `transport.py`'s `REPORT_SIZE`/`message_to_reports()`, which
allocates `1 + REPORT_SIZE`). Both are plain C++23 with no Zephyr dependency at all — verified
by compiling and running their test cases directly with the system's `clang++` (no Zephyr SDK
needed) before porting the same cases into the Ztest suite below.

**Correction, found and confirmed during #7's hardware verification**: this was originally
implemented as 128 bytes *total* (report id counted as part of the 128), one byte short of
qc-mcp's actual wire format. On real hardware this produced a spurious trailing 1-byte "report"
after every legitimate message - the device's actual 129-byte input report was being split by
our too-small receive buffer (`QC_USBH_REPORT_SIZE`) into a 128-byte transfer plus a 1-byte
leftover, which our strict "exactly `kHidReportSize` bytes or it's malformed" check then
rejected. Corrected `kHidReportSize` and `QC_USBH_REPORT_SIZE` to 129 to match `transport.py`
exactly; re-verified on hardware - each input report now arrives as a single 129-byte transfer,
no trailing fragment. (The handshake's outgoing `SET_REPORT` also failed at the same time, with
`-EPIPE` - that turned out to be unrelated to this off-by-one; see #7's section below.)

Run the tests:

```bash
west twister -p native_sim -T firmware/app/tests/hid_framing
```

**`native_sim` only runs on a Linux host** — it uses actual Linux syscalls, not a portable
POSIX layer, so on macOS this will always report "FILTERED: Native platform requires Linux."
That's expected, not a bug; it's what CI (presumably a Linux runner) will use. For local
testing on macOS, use a QEMU-emulated target instead, overriding the testcase's
`platform_allow` for this one run:

```bash
west twister -p qemu_cortex_m3 -K -T firmware/app/tests/hid_framing
```

**Verified 2026-09-15**: all 7 test cases pass on `qemu_cortex_m3`. Needed one additional fix
along the way — `CONFIG_REQUIRES_FULL_LIBCPP=y` (now in both `prj.conf` files), since Zephyr's
default `MINIMAL_LIBCPP` doesn't provide `<span>` (or `<cerrno>` — see `UsbHostClass.hpp`'s
history) at all. Confirmed against the pinned v4.4.2 `lib/cpp/Kconfig` source, not assumed.

## HID report transport (issue #4, part 2 of 2)

`usbh_shim.c` now discovers interface 5's actual IN/OUT endpoint addresses (walking its
descriptors via `usbh_desc_get_next()` until the next interface descriptor, right after
`onProbe()` accepts the device) and starts a continuous receive loop on the IN endpoint:
allocate a report-sized buffer, submit it, and on every completion hand the bytes to
`on_report_in()` and immediately re-arm with a fresh buffer. `qc_usbh_send_report()` submits
one report on the OUT endpoint the same way, for whenever there's something to send (that's
issue #7 — nothing calls it yet).

This is modeled directly on `subsys/usb/host/class/usbh_uvc.c`'s `initiate_transfer`/
`continue_transfer` pattern — the only in-tree, merged USB host class driver that actually
moves data, and the closest thing to a proven reference for this part of the API. Every
field/function name (`xfer->buf`, `xfer->err`, `usbh_xfer_alloc`, `net_buf_add_mem`, ...) was
checked against the pinned v4.4.2 source before use, not assumed from newer docs.

`QcHidBridge` now owns a `HidReassembler` and feeds every received report into it, logging
the byte count on each successfully reassembled message.

**Verified 2026-09-15** against a real Quad Cortex Mini:

```
<inf> main: Claimed Quad Cortex Mini HID interface 5
<inf> usbh_shim: Found endpoint 0x81, attributes 0x03, wMaxPacketSize 128
<inf> usbh_shim: Interface 5 endpoints: IN=0x81 OUT=0x00
<inf> usbh_shim: Starting receive loop on IN endpoint 0x81
```

No data arrived while the device sat connected — expected, not a gap: qc-mcp's protocol
means the device stays silent until the session handshake (#7) exists, so this is the
*correct* result, not a missing feature. Disconnecting the device produced:

```
<wrn> usbh_shim: IN transfer completed with error: -5
```

`-5` is `-EIO` — the pending transfer correctly erroring out on physical disconnect, which
also confirms the completion/error-handling path actually works (not just the happy path).
Reconnecting cleanly re-ran the whole enumerate → claim → discover → receive sequence.

Between endpoint discovery working, the receive loop starting, and the disconnect exercising
real error handling, every phase of #4's transport plumbing has now been exercised against
real hardware — the remaining gap (actually reading a reassembled message) needs the session
handshake from #7, which is genuinely out of scope here, not a shortfall of this issue.

**Confirmed limitation, feeds directly into #7**: interface 5 has exactly one endpoint (the
IN one above) — no interrupt OUT endpoint exists. `qc_usbh_send_report()` as currently
written always fails with `-ENODEV`; sending will need HID's `SET_REPORT` control transfer
on endpoint 0 instead. Documented in `usbh_shim.h` and on issue #7.

## Session handshake (issue #7)

[`QcSession.hpp`](app/src/QcSession.hpp) sends `ResetCommsBuffers` UPDATE, `Version` READ
then UPDATE (mirroring the device's own reported firmware version, falling back to a fixed
string if no reply arrives in time), and `Connection` UPDATE, per `PROTOCOL.md` section 3.
`qc_usbh_send_report()`'s `SET_REPORT` control transfer (`usbh_shim.c`) is what actually
gets the handshake messages onto the wire, since interface 5 has no interrupt OUT endpoint
(see above).

**Verified on real hardware**: the handshake completes, mirrors the device's real reported
firmware version (`4.1.1`, not a fallback), and the device answers subsequent `Version`
READs reliably while the KeepAlive heartbeat (issue #8, below) is running.

**`SET_REPORT` returns `-EPIPE` on every send to this device**, confirmed on real hardware.
This isn't a parameter bug — the device's own HID report descriptor (`05 01 09 00 a1 01 15
00 26 ff 00 85 01 75 08 95 80 09 00 81 82 85 02 75 08 95 80 09 00 91 82 c0`, decoded: Report
ID 1 = 128-byte Input, Report ID 2 = 128-byte Output) matches `qc_usbh_send_report()`'s
construction exactly (wValue `0x0202`, wLength 129); `GET_REPORT` succeeds cleanly for both
report IDs, only the OUT direction stalls. `PROTOCOL.md` §1 documents the same thing one
layer up: *"macOS quirk: `IOHIDDeviceSetReport` returns a benign `0xe0005000` on this device
— the official app ignores it too, and the data is still sent. Not an error."*
`QcSession::sendMessage()` treats `-EPIPE` as non-fatal and continues. This is now confirmed
functionally, not just inferred: repeated `Version` READs reliably got answered for several
minutes with the heartbeat running, every single send still returning `-EPIPE` throughout.

**Correction**: an earlier version of this section (and of issue #53's hardware-verification
commit) read the device's response right after the handshake as an autonomous "burst of
state READs," offered as proof the handshake's sends had landed. Issue #8's later hardware
debugging decoded that response's actual command ids and found it is **entirely the
(delayed) `Version` reply itself** — a `331`-byte and a `10`-byte message, both command
`10`, with (on the very first request only) an extra `14`-byte one. It wasn't a state
stream at all, and the real explanation for why it looked delayed is below.

**`kVersionReplyTimeout` was 500ms and too short**, a bug present since this issue but only
caught while debugging #8: this device's actual `Version`-reply latency runs a few hundred
ms, past that window, so the handshake's wait reliably logged `gotReply=0` and fell back to
a hardcoded firmware string even though the device always did reply, just late. Fixed by
widening the wait to 1000ms (`QcSession.hpp`) — confirmed on hardware: the handshake now
reports `4.1.1`, the device's real version, instead of the `4.1.0` fallback.

## KeepAlive heartbeat (issue #8)

[`QcSession.hpp`](app/src/QcSession.hpp) sends a `KeepAlive` UPDATE (`action=UPDATE,
is_online=true`) every ~200ms, on a dedicated `k_timer` + thread so a slow or blocked send
never stalls the handshake or vice versa: the timer's expiry callback only signals a
semaphore (it runs in interrupt-adjacent context and must not block - CODING_STANDARDS.md's
ISR safety rules), and the heartbeat thread does the actual blocking `SET_REPORT` send.
`QcSession::sendMessage()` — the shared USB send path for the handshake and heartbeat
threads — is guarded by a `k_mutex` ([`ScopedKMutex`](app/src/ScopedKMutex.hpp)) so their
HID reports can never interleave on the wire, per the issue's explicit requirement.
`KeepAliveMessage`'s `request_id`/`timeout` fields are deliberately left unset in
`QcHandshakeMessages.hpp::encodeKeepAlive()` — qc-mcp's own reference implementation never
sets them either, so they never appear on the wire.

**The heartbeat is started *before* the handshake sends anything**, not after — matching
qc-mcp's own `transport.py::open()` order (`_start_heartbeat()` then `_handshake()`, with an
explicit comment that the heartbeat is what "keep[s] session online (required for reads)").
An earlier version of this implementation got this backwards, leaving a window with nothing
keeping the session alive before the first `Version` READ was even sent — found while
diagnosing why that READ never got a reply in time (see #7's section above for the related
500ms-timeout bug this surfaced).

### What was verified on real hardware

With both fixes above in place: the handshake completes and mirrors the device's real
firmware version (`4.1.1`); a repeated `Version` READ, sent periodically while the heartbeat
ran, was reliably answered for several minutes straight.

### What this does *not* show

- **No heartbeat-off comparison was run.** The claim that reads require the heartbeat
  (issue #8's whole premise) rests on `PROTOCOL.md`'s own documentation of the device's
  behavior ("reads require it"), not on a direct A/B test against this firmware with the
  heartbeat disabled. Worth doing if this ever needs re-confirming: comment out the
  `startHeartbeat()` call at the top of `runHandshake()`, reflash, and confirm reads then
  fail where they currently succeed.
- **No preset-change push was observed at the time this section was written**
  (`RecallPreset`/`Scene`/`SetlistPosition`/`PresetDirty`/`RecentsFavorites`, per
  `PROTOCOL.md`'s description of what a device-side preset recall autonomously pushes). The
  candidate explanation then was qc-mcp's own `transport.py::_handshake()` sending twelve
  explicit subscription READs right after `Connection` UPDATE, with a comment that this is what
  "makes the QC start streaming" — this firmware hadn't sent any of those yet. **Resolved in
  issue #61**: sending those twelve READs (see the #60/#61 section below) made the device both
  answer the File READ and push a compressed `RecallPreset` on a preset change, confirmed on real
  hardware.

### The tested build differs from this one

The multi-minute hardware run described above (handshake mirrors `4.1.1`, repeated `Version`
probes answered for several minutes) was against a build carrying temporary diagnostics on
top of the same fixes — per-send result logging, a periodic probe read, a recent-messages
ring buffer, an IN-transfer counter, and `THREAD_ANALYZER` reporting. Those were removed in
the cleaned-up commit (`f8ad423`) now that what they were added to determine is settled; the
underlying fixes they helped find (heartbeat before handshake, the 1000ms `Version`-reply
wait, the send mutex) are unchanged.

**Verified on `f8ad423`** against a real Quad Cortex Mini (the smoke test below): boot,
handshake mirrors `4.1.1` with no `No usable Version reply` warning beforehand, and no
crash over a couple of minutes. The longer-duration repeated-reads result above is still
only confirmed on `584e015`, the diagnostics-carrying build, not on this cleaned-up one — a
heartbeat-off comparison and a preset-change push both remain unconfirmed on either build.

### Smoke test for this build

1. Flash and open the serial console (see Verify above).
2. Confirm the boot banner, USB enumeration, and interface claim as in the traces above.
3. Confirm `Session handshake complete (firmware version: 4.1.1)` — **not** a `No usable
   Version reply` warning line beforehand, and **not** the `4.1.0` fallback.
4. Confirm `KeepAlive heartbeat started (~200ms)` logs once, right after.
5. Leave it running for a couple of minutes: no crash, no fatal error, no further `LOG_ERR`
   lines (`Handshake: failed to send ...` or `sendReport failed`).

### A lasting lesson from this debugging, for future changes here

Zephyr's own internal `usbh` thread (the one that calls `onProbe()`/`onReportIn()`, see
`UsbHostClass.hpp`) has a small stack (`CONFIG_USBH_STACK_SIZE`, now 1536 bytes — see
`prj.conf`'s comment for the measured number behind that), and this project's
`CONFIG_LOG_MODE_IMMEDIATE=y` means any `LOG_*` call runs its formatting synchronously on
the calling thread's own stack. A diagnostic log call added to
`QcSession::handleMessage()` — which runs on that thread — crashed real hardware with a
stack overflow. Keep that function (and anything else reachable from `onProbe()`/
`onReportIn()`) free of logging and other stack-heavy calls; do real work from one of this
project's own threads instead.

## Zephyr 4.5 upgrade (issue #54)

Bumped the pinned release from v4.4.2 to v4.5.0 (released 2026-10-28). As scoped in #54,
this surfaced exactly one break in code this project depends on, not the zero predicted by
the header diff that scoped the issue: `usbh_device.h`'s `usbh_xfer_free()` wrapper was
removed upstream ([`usb: host: replace usbh_xfer_free() with uhc_xfer_unref()`](https://github.com/zephyrproject-rtos/zephyr/commit/b7b06c6740c4425fd15dc3226f3688dee453669d),
landed after the API diff #54 checked was done) — the underlying `uhc_xfer_free(dev, xfer)`
is still there, but the convenience wrapper our shim called is gone in favor of a ref-counted
`uhc_xfer_unref(xfer)` (no `dev` argument). `usbh_shim.c`'s `start_receiving()` error path
was the one remaining call site after rebasing onto #7/#53's session-handshake work, which
had already replaced the old async OUT-endpoint send path (and its own two call sites) with
`qc_usbh_send_report()`'s `usbh_req_setup()` control transfer above; every other API this
project touches (`usbh_xfer_alloc()`, `usbh_desc_get_next()`/`usbh_desc_get_iface()`, the
`usbh_class_data`/`usbh_class_filter` structs) was unaffected, matching what #54 found.

Rebuilt `frdm_rw612` clean against the v4.5.0 workspace (succeeds), and re-ran both Ztest
suites (`hid_framing`, `protocol_messages`) on `qemu_cortex_m3` — all 24 cases still pass.
`frdm_rw612` and `CONFIG_STD_CPP23` both still exist in v4.5.0, confirmed directly against
the checked-out source, not assumed.

**Verified 2026-10-31** against a real Quad Cortex Mini on the v4.5.0 build: the full
enumerate → claim → discover → receive → session-handshake flow this file documents above
still works end-to-end.

## Tier 2 protobuf decode: ModelRepo, SetlistPosition (issue #5)

[`QcModelRepoMessage.hpp`](app/src/QcModelRepoMessage.hpp) and
[`QcSetlistPositionMessage.hpp`](app/src/QcSetlistPositionMessage.hpp) add `decodeModelRepo`/
`decodeSetlistPosition`, built directly on ADR 0003's existing `QcProtobuf.hpp` primitives -
no changes to that file were needed. Checking both messages against qc-mcp's real descriptor
set (rather than the nested/repeated-field shape issue #5 originally assumed) found they're
flat and scalar-only, same as every Tier 1 message; see
[ADR 0004](../docs/adr/0004-tier2-protobuf-codec.md) for the full reasoning and why nanopb is
still deferred. New Ztest suite: `tests/catalog_messages` (kept separate from
`tests/protocol_messages` to avoid touching a file issue #8 is also working in).

`ModelRepoMessage.model_repo_payload` decodes to a `std::span` into the caller's buffer, not a
copy - it's a gzip+tar of `ModelRepo.xml` (the device catalog), still opaque bytes until issue
#6 inflates it. **Not yet verified against real device traffic** - no hardware run was done for
this issue; tests use constructed bytes built from the verified field numbers, not a capture.
Real `ModelRepo` traffic also needs the HID reassembly buffer (`kReassemblyCapacity`, still
issue #33's placeholder 4096 bytes) sized to hold the *whole* compressed catalog before
`decodeModelRepo` ever sees a complete message - that sizing work belongs to #33, not this
issue.

Run the tests the same way as `hid_framing` (see above):

```bash
west twister -p qemu_cortex_m3 -K -T firmware/app/tests/catalog_messages
```

## File message decode: folder and preset names (issue #59)

[`QcFileMessage.hpp`](app/src/QcFileMessage.hpp) adds `decodeFile()`, the first Tier 2 message
with real nested/repeated structure (`FolderInfo`, and its repeated `files`) - still built
directly on `QcProtobuf.hpp`'s existing `ProtoFieldReader`, recursed per nesting level, with no
changes to that file. See [ADR 0006](../docs/adr/0006-file-message-codec.md) for why nanopb is
still deferred even here, and for the callback-based (not reply-struct) shape this decoder
uses. New Ztest suite: `tests/file_messages`.

Only `type == 0` (Presets) folders produce callbacks - IRs/Captures folders in the same stream
decode successfully with zero callbacks, matching qc-mcp's `directory.py::structure_directory()`
filtering. **Not yet verified against real device traffic** - tests use constructed bytes built
from the verified field numbers in the ADR, not a capture. Wiring `decodeFile` into the
session/message-dispatch path (the actual `File` request) is issue #10's job, not this one's.

Run the tests the same way as `hid_framing`/`catalog_messages` (see above):

```bash
west twister -p qemu_cortex_m3 -K -T firmware/app/tests/file_messages
```

## Message dispatch, the File stream, and post-connect state READs (issues #60, #61)

General message dispatch: [`QcSession::handleMessage()`](app/src/QcSession.hpp) now hands every
reassembled message to either [`QcGzipInflater.hpp`](app/src/QcGzipInflater.hpp) (issue #6, if
the gzip magic prefix is present) or the new [`QcDispatcher.hpp`](app/src/QcDispatcher.hpp)
(otherwise) - both single-slot-mailbox-plus-worker-thread classes, the same shape, so a message
arriving while the previous one on that path is still being handled is dropped rather than
queued. `handleMessage()` itself still does only a cheap trailer decode and a 3-byte gzip-magic
check: no logging, no real dispatch logic, matching the lesson baked in since issue #8 (see
above) about Zephyr's `usbh` thread stack. Both worker threads call back into `QcSession`
through a small interface, [`QcMessageSink.hpp`](app/src/QcMessageSink.hpp)
(`onMessage()`/`onInflateFailed()`), from their own thread - never from `usbh` - so that's where
real dispatch logic (today: recognizing a Version reply, and the File hook below) safely lives.

Post-connect state READs (issue #61): right after the handshake's Connection UPDATE and before
the File READ below, `QcSession::runHandshake()` now sends the same twelve state READs qc-mcp's
own `transport.py::_handshake()` sends at that point -
[`QcStateReads.hpp`](app/src/QcStateReads.hpp)'s `kPostConnectStateReadCommands`, in the exact
order qc-mcp uses: `ModelRepo`, `ModuleStats`, `UndoRedo`, `IOSettings`, `GeneralSettings`,
`Mode`, `GlobalEQ`, `MasterVolume`, `GlobalTempo`, `Scene`, `PresetDirty`, `SetlistPosition`.
qc-mcp's own comment on this step says plainly that it's what "makes the QC start streaming and
marks the session active" - the hardware run on commit `11e43d0` (issue #60's hardware debugging,
below: a 30-second timeout with only one message the whole window, the handshake's own delayed
`Version` reply, and no File traffic at all) made these the leading candidate explanation.
**Confirmed on commit `0d1edcc`** (see "Hardware results" below): with these twelve READs in
place, the same device answered the File READ for the first time. The earlier framing of this as
an unproven candidate - or of a bare File READ possibly being enough on its own - no longer
applies to this device.

Command ids (protocol.py's `COMMANDS` map) and field numbers (qc_descriptors-4.1.pb) were taken
directly from qc-mcp's source, not guessed: every one of the twelve message types shares the same
two leading fields, 1=`action` and 2=`request_id`, confirmed by decoding the descriptor set
itself field-by-field rather than assumed from the Tier 1/File shape already in this codebase -
so one generic encoder, `encodeStateRead()`, covers all twelve. Sent back-to-back with no wait for
a reply in between, and no wait for any of their replies before moving on to the File READ either
- that mirrors qc-mcp exactly: its own loop fires all twelve, then only pauses briefly
(`time.sleep(0.3)` + a short `_collect()`) before discarding whatever already came back, never
gating on a specific reply. This firmware mirrors that fire-and-forget shape rather than inventing
a synchronous wait qc-mcp doesn't have. A send failure on any one of the twelve logs a warning and
moves on to the next (and still to the File READ afterward) rather than aborting the sequence -
matching qc-mcp's own per-READ `try`/`except`.

Replies to these - including a `ModelRepo` reply that's large and/or gzip-compressed - are handled
by the same generic dispatch path as every other message
([`QcDispatcher.hpp`](app/src/QcDispatcher.hpp)/[`QcGzipInflater.hpp`](app/src/QcGzipInflater.hpp),
described below), which already drops-and-counts rather than crashes on anything too big for
`kReassemblyCapacity` or either mailbox - no new handling was needed for this issue's "don't crash,
don't block the File READ" requirement, since issue #60 had already made that path crash-safe for
an unknown command's reply of unknown size. A `ModelRepo` reply overflowing `kReassemblyCapacity`
(4096 bytes) shows up as `reassemblyOverflowCount` in the File stream's summary below, same as any
other oversized message during that window.

The File request: [`QcFileRequest.hpp`](app/src/QcFileRequest.hpp) encodes a bare File READ
(command 4), sent right after the handshake completes (`QcSession::runHandshake()`). The device
answers with a stream of File UPDATE messages covering its whole catalog (presets, IRs,
captures) - qc-mcp's `transport.py::list_directory()` describes about 400 messages over about
12s on a real device. [`QcFileStream.hpp`](app/src/QcFileStream.hpp) collects that stream: a
1500ms quiet window or a 30s overall cap, whichever comes first, mirroring that function's
`quiet_ms`/`timeout_ms` defaults. It's plain C++23 with no Zephyr dependency, same reasoning as
`HidReassembler.hpp` - the whole state machine takes an explicit `nowMs` rather than calling
`k_uptime_get()` itself, so it's unit-testable with synthetic timestamps and no real waiting
(see `tests/dispatch_messages`). `QcSession` calls it under its own mutex (`fileStreamMutex_`)
from `onMessage()`/`onInflateFailed()` (the dispatch/inflate worker threads) and from
`runHeartbeat()`, which polls `checkTimeout()` on its own ~200ms cadence rather than running a
separate timer - plenty of resolution for a 1500ms window, and keeps the check off `usbh` too.

The File-message hook (issue #60's scope item 3, ahead of #10 wiring in the sibling issue #59's
decoder): for now this only logs command, wire size, inflated size, and whether a message was
compressed - at `LOG_DBG`, not `LOG_INF`, since ~400 of these would otherwise drown everything
else in the log during the stream (the existing per-report "Reassembled message" log in
`QcHidBridge.hpp` was downgraded to `LOG_DBG` for the same reason). The readable, once-per-stream
version is `QcFileStream`'s summary, logged in two `LOG_INF` lines when the stream finishes: the
first has message/compressed/failed counts, total wire and inflated bytes, and elapsed time; the
second has the peak sizes and the data-loss figures below - together, what feeds issue #33's
eventual sizing work.

**Known gaps, surfaced rather than fixed here.** Two separate things can make the summary
undercount what the device actually sent, and both are counted and folded into the summary
instead of only showing up as scattered log lines:

- `QcDispatcher` and `QcGzipInflater` each have a single in-flight mailbox slot (see their own
  header comments) - a message arriving while the previous one on that path is still being
  processed, or one too big for the 4096-byte mailbox, is dropped. Both count every drop
  (`droppedCount()`, a lifetime total); `QcSession` snapshots that count when the File stream
  starts and diffs it at the end, so the summary's `dispatcherDropped`/`gzipDropped` report only
  what was lost *during this stream*. **Measured on real hardware (commit `0d1edcc`): both 0**
  across a 551-message, ~12s stream - the single-slot design kept up at this device's actual
  arrival rate; no evidence yet to justify a bounded queue.
- A File folder message that overflows `QcHidBridge`'s `kReassemblyCapacity` (4096 bytes) is
  dropped by `HidReassembler` *before* `handleMessage()` ever sees a command id, so - unlike the
  two drops above - it can't be attributed to the File stream specifically; the same goes for a
  malformed HID report. Both are counted (`reassemblyOverflowCount`/`reassemblyMalformedCount`,
  same snapshot-and-diff treatment) and folded into the summary as a best-effort "this happened
  somewhere during the stream" figure, rather than left solely to a log line. These used to log
  via `LOG_WRN` directly in `QcHidBridge::onReportIn()` - run on the `usbh` thread, where (unlike
  the already-filtered-out-by-default `LOG_DBG` calls nearby) an *enabled* level like `LOG_WRN`
  really does run `cbprintf` formatting synchronously on that 1536-byte stack, the same pattern
  that overflowed it once before (see issue #8's section above). Switched to atomic counters
  instead, with no logging on that thread at all for these two cases now. **Measured on real
  hardware (commit `0d1edcc`): overflow=31, malformed=2850, out of 8843 total IN transfers - not
  zero.** See "Hardware results" below for what that means and why it's deferred to issue #33
  rather than fixed here.

A File message that decodes far enough to detect gzip but then fails to inflate (e.g. exceeds
`QcGzip.hpp`'s `kMaxInflatedSize`, 16384 bytes) is counted differently again - via
`onInflateFailed()`/`failedCount`, since by that point a command id and a wire size *are* known.
All of the 4096/16384-byte limits above are issue #33's placeholders to size for real, not
something this issue changes; the summary's `maxWireBytes`/`maxInflatedBytes` fields (the
largest single message actually seen, successfully processed - a message dropped for being too
big never reaches this tracking at all) are what #33 needs for that sizing pass. This PR adds no
overflow-size measurement beyond the existing overflow/malformed counters, and does not change
`kReassemblyCapacity` or `kMaxInflatedSize` - sizing them for real is #33's job, not this one's.

**Hardware results (commit `0d1edcc`, the state-READs commit; logging cleanup after it is not
yet separately re-verified - see "Smoke test for this build" below).** A real Quad Cortex Mini:

```
File stream finished (quiet): 551 messages (0 compressed, 0 failed), 586523 wire bytes, 582115 inflated bytes, 11676 ms
File stream limits: max wire=3946 max inflated=3938, dropped (dispatch=0 gzip=0), reassembly (overflow=31 malformed=2850), 8843 total IN transfers
```

followed, about 25 seconds later, by a compressed `RecallPreset` (15) - "Inflated command 15: 1223
-> 13235 bytes" - together with `Scene` (13), `Grid` (1), and `RecentsFavorites` (20). `GlobalTempo`
(33) streamed throughout the whole session at roughly 4 times a second. No crash.

**Confirmed:**

- Issue #61's post-connect state READs were the missing step: this is the same device that, on
  commit `11e43d0` (without the state READs), got a live-but-silent session - one message in 30
  seconds, the handshake's own delayed `Version` reply, no File traffic at all. With the state
  READs in place, the File stream completed. The earlier "maybe a bare File READ is enough on its
  own" framing is gone - on this device, it isn't.
- `QcGzipInflater` inflates real compressed hardware traffic correctly: the `RecallPreset` above
  is the first gzip-compressed message seen on real hardware, and it decompressed without issue.

**Not complete, deferred to issue #33:** 31 messages overflowed `kReassemblyCapacity` (4096
bytes) and were lost before a command id could even be read, so which command(s) they belonged to
is unknown. `malformed=2850` is almost certainly their continuation reports - once an overflow
resets `HidReassembler` mid-message, every MIDDLE/LAST report that follows with no preceding FIRST
decodes as malformed, until the next FIRST report resyncs it. Some of the 31 lost messages may
have been preset folders, so the File catalog this stream collected is **not known to be
complete**. The owner's preset change wasn't precisely timed, so the ~25s gap before `RecallPreset`
arrived is inferred, not measured against a stopwatch.

**Not yet verified on real hardware: this cleaned-up build itself.** The result above is from
commit `0d1edcc`, before this PR's logging cleanup (quieter post-connect tally output,
`QcMessageTally`'s capacity fix - see below). Neither changes what the device is sent or how
File/state-READ replies are handled, so the result shouldn't change, but it hasn't been re-run.

**Smoke test for this build:**

1. Flash this build and open the serial console (see Flash/Verify above).
2. Confirm the handshake completes (`Session handshake complete (firmware version: ...)`, no `No
   usable Version reply` warning).
3. Confirm the File stream finishes quiet (`File stream finished (quiet): ...`, not `(timeout)`).
4. Change the loaded preset once on the unit and confirm a `Post-connect tally: command=...` line
   appears for it (`RecallPreset`/`Scene`/`Grid`/`RecentsFavorites`/similar).
5. Let it run a couple of minutes: no crash, and no flood of repeated tally lines (`GlobalTempo`'s
   continuous ticking should produce at most one post-connect tally line per minute - see below).

A finished stream logs several lines - paste back all of them:

```
File stream finished (quiet): N messages (C compressed, F failed), W wire bytes, I inflated bytes, E ms
File stream limits: max wire=MW max inflated=MI, dropped (dispatch=DD gzip=DG), reassembly (overflow=RO malformed=RM)
File stream tally: T total IN transfers
File stream tally: command=CMD count=CNT    (one line per distinct command id seen, or "no messages of any command seen in this window")
Post-connect tally started: watching for preset-change pushes (RecallPreset/Scene/SetlistPosition/...) - issue #61
```

`QcMessageTally.hpp` (`QcSession`'s `messageTally_`) records every message handed to
`onMessage()`/`onInflateFailed()` by command id, not just File, for the lifetime of one File
stream - added so a run with zero File messages could tell "the device sent nothing at all" apart
from "the device sent something, just not File" (see the `11e43d0` result above). `T`
(`QcSession::totalInTransfers_`, incremented in `QcHidBridge::onReportIn()`) counts one layer
lower still - every successful USB IN transfer, regardless of whether the reassembler ever turned
it into a counted message at all.

`QcMessageTally`'s capacity (`kMaxDistinctCommands`) was 8 through commit `0d1edcc`, and the
`0d1edcc` hardware run above hit that limit: the twelve state-READ replies alone can fill all 8
slots before File's own first message arrives, so an entire ~550-message File stream was counted
as "other" instead of under command 4 ("554 messages from other distinct commands (> 8 tracked)").
Fixed in this PR by raising the cap to 24 (comfortably above the 18 distinct command ids this
firmware can see in one connect - the twelve state-READ replies, File, Version, and the five
preset-change-push commands below) - static, no behavior change beyond no longer losing File to
the limit.

The last line starts a second, separate tally (`QcSession`'s `postStreamTally_`, same
`QcMessageTally.hpp` class, a different instance) that keeps running for the rest of the session,
specifically for issue #61's preset-change check: `messageTally_` above stops dead the moment the
File stream finishes, so without this there'd be no visibility into anything the device sends
afterward - including a preset-change push. It logs a `Post-connect tally: command=CMD count=CNT`
line (same per-command shape as the File stream's own tally lines, plus a similar "other distinct
commands" line past the 24-command cap) only when something actually worth noticing happens:
immediately for a command id never seen before in this window (the real preset-change signal -
`RecallPreset`/`Scene`/`Grid`/`RecentsFavorites`/`SetlistPosition`/`PresetDirty`), and at most once
a minute for a plain count bump on an already-seen command, reprinting only the command(s) that
actually changed rather than the whole tracked set. That one-minute throttle exists because
`GlobalTempo` (and `CPULoad`, per qc-mcp's own comment) streams continuously under an active
heartbeat whether or not a preset ever changes (confirmed above: ~4/s for the whole session) -
without it, every tracked command got reprinted once a second, GlobalTempo's ticking alone
swamping the log for the rest of the session and burying the one line that actually mattered. If
nothing new happens after the stream, you'll simply see no further `Post-connect tally: ...`
lines.

What a few of the summary fields mean, for issue #33's sizing work:

- **`DD`/`DG` (dispatcherDropped/gzipDropped) above 0**: the single-slot mailbox in
  `QcDispatcher`/`QcGzipInflater` couldn't keep up with the device's arrival rate during this
  stream. 0 on the hardware run above; `N` is then a lower bound on what the device actually sent
  if this is ever nonzero on a future run.
- **`RO`/`RM` (reassemblyOverflowCount/reassemblyMalformedCount) above 0**: confirmed nonzero on
  real hardware (31/2850) - see "Hardware results" above.
- **`MW`/`MI` (maxWireBytes/maxInflatedBytes)**: the largest single message actually seen,
  successfully processed. If `DD`/`DG`/`RO` are all 0 this is the true peak; otherwise the true
  peak is *at least* this large, since a dropped message never reaches this tracking.
- **A crash or stack fault**: capture the fault PC/LR and `addr2line` against `zephyr.elf`, same
  method used to find issue #8's `usbh`-stack overflow (see that section above) - and say which
  thread's name appears in the dump (`qc_dispatch`, `qc_gzip_inflate`, `qc_session`,
  `qc_heartbeat`, or `usbh` itself).

## Reassembly/inflate overflow sizing, step 1: measurement (issue #33)

Issue #33 predates ADR 0003/0004 and issue #6: there are no nanopb buffers to size (this
project uses a hand-written codec - see the issue's latest comment), and the real placeholders
needing real numbers are `kReassemblyCapacity` (4096, `QcHidBridge.hpp`) and `kMaxInflatedSize`
(16384, `QcGzip.hpp`). The #60/#61 hardware run above measured both as too small for real
traffic - `reassembly (overflow=31 malformed=2850)` out of 8843 IN transfers, and a later run
(commit `f0a4939`) hit `kMaxInflatedSize` too: a compressed `RecallPreset` (command 15, 2408
compressed bytes) failed with `OutputTooLarge`. Which command(s) were involved, and how big
they really are, was unknown either time - **this is step 1: measure that, by command, without
choosing new sizes yet.** Step 2 (picking real sizes from that data, and deciding whether a very
large message type like `ModelRepo` should be ignored outright rather than buffered) is a
separate, later change - `kReassemblyCapacity`/`kMaxInflatedSize` are unchanged by this one.

**`HidReassembler::Status::Overflow` no longer aborts the moment Capacity is exceeded.** Before
this change, the first report that pushed a message over Capacity reset the reassembler
immediately and returned `Overflow`, discarding all knowledge of the message - every later
MIDDLE/LAST report belonging to it then had no preceding FIRST and miscounted as a *second,
unrelated* `Malformed`. This is almost certainly the real explanation for `malformed=2850`
above (2850 is close to what a handful of these ~4000-byte-overflowing messages' remaining
reports would add up to). Now `HidReassembler` keeps consuming reports for an overflowing
message - without growing the stored body past `Capacity` - until its LAST/SINGLE report
arrives, tracking the message's true total size (`overflowSize()`) and retaining only its last
8 bytes (`overflowTrailer()`). Those last 8 bytes are always `QcMessage.hpp`'s message trailer
(`[command][reserved][hash]`, always the final bytes of a message), so
`decodeTrailerCommand()` (`QcMessage.hpp`) can still say which command overflowed even though
its body was dropped. New Ztest cases in `tests/hid_framing` cover the overflow-then-complete
sequence, the true size/trailer it reports, resync on the next FIRST, a malformed report
arriving mid-overflow, and that continuation reports after an overflow are no longer
`Malformed`.

**Per-command reporting, added to the File stream's summary:**

- `QcHidBridge::onReportIn()`'s `Overflow` case now decodes the command from `overflowTrailer()`
  and calls `QcSession::recordReassemblyOverflow(command, trueSize)` - still atomic-increment-only,
  no logging, same `usbh`-thread safety contract as before (see that function's comment: this
  thread's 1536-byte stack with `CONFIG_LOG_MODE_IMMEDIATE` means an enabled log level really
  does run `cbprintf` formatting synchronously on it - the exact pattern that crashed real
  hardware once already, issue #8). The per-command breakdown lands in a new, lock-free
  `QcReassemblyOverflowTally` (`QcReassemblyOverflowTally.hpp`) - plain C++23, `std::atomic`
  only, no mutex, since `usbh` is the sole writer and the only safety requirement is that the
  heartbeat thread's read doesn't race it.
- `QcMessageTally.hpp`'s `Entry` gained `maxWireBytes`/`maxInflatedBytes`, and `record()` takes
  optional wire/inflated sizes - so the existing per-command tally (`messageTally_`, scoped to
  one File stream) now also reports each command's own peak sizes, not just its count.
- A message that fails to inflate because it's too big for `kMaxInflatedSize`
  (`gzip::InflateError::OutputTooLarge`) still has a real, known decompressed size: gzip's own
  trailer carries it (the ISIZE field, RFC 1952) at a fixed offset from the *end* of the
  compressed bytes, readable without finishing - or even attempting - decompression. New
  `gzip::peekDecompressedSize()` (`QcGzip.hpp`) reads it directly; `QcGzipInflater.hpp` calls it
  on every inflate failure and passes the result through `QcMessageSink::onInflateFailed()`'s
  new `trueInflatedSize` parameter, which `QcSession` folds into both `messageTally_`'s
  per-command `maxInflatedBytes` and `QcFileStream`'s own summary-wide `maxInflatedBytes` - so a
  `RecallPreset` that was too big to actually inflate still shows its true size in both places,
  not just a `failedCount` bump with no size attached.

**What the stream-finish log block now looks like** (new lines in **bold**, relative to the
#60/#61 section above):

```
File stream finished (quiet): N messages (C compressed, F failed), W wire bytes, I inflated bytes, E ms
File stream limits: max wire=MW max inflated=MI, dropped (dispatch=DD gzip=DG), reassembly (overflow=RO malformed=RM)
File stream tally: T total IN transfers
File stream tally: command=CMD count=CNT max_wire=MWC max_inflated=MIC   (one line per distinct command, or "no messages of any command seen in this window")
File stream overflow by command: command=CMD count=CNT max_true_size=MTS   (one line per command that ever overflowed, or "none")
Post-connect tally started: watching for preset-change pushes (RecallPreset/Scene/SetlistPosition/...) - issue #61
```

`max_wire`/`max_inflated` on the per-command tally line are that *specific* command's own peak
sizes (`MW`/`MI` on the line above remain the peak across every command, unchanged). The new
overflow-by-command line(s) are lifetime counts, not diffed to this one stream's window the way
`RO`/`RM` are - this firmware does one connect per boot, so since-boot and since-this-stream are
the same thing in practice, and it wasn't worth a second, atomics-only snapshot-and-diff scheme
for a count this rare (31 across a whole connect's ~8800 IN transfers). If `File stream overflow
by command` ever shows `> 16 tracked` falling into "other distinct commands", that's worth a
comment on #33 before trusting the per-command numbers - 16 is sized for "a handful of large
commands overflow," not an arbitrary count.

**Hardware test - run this on the next flash:**

1. Flash this build and let the handshake, state READs, and File stream run exactly as the
   #60/#61 smoke test above describes.
2. Paste back the full stream-finish log block above, including the new `File stream overflow
   by command: ...` line(s).
3. If a preset change pushes a compressed message that's too big to inflate (the `f0a4939`
   hardware run's `RecallPreset` case), paste the `Inflate failed for command ...` line too - it
   now includes `true inflated size NNNN` when the trailer was readable.

**What each outcome means:**

- **`File stream overflow by command` lists one or more real commands (e.g. `command=51
  count=1 max_true_size=XXXXX`)**: that command's true size is now known - #33's step 2 can size
  `kReassemblyCapacity` to it directly (or decide to deliberately exclude that command instead,
  if its true size is large enough that buffering it outright isn't worth the RAM - the
  `ModelRepo` case the issue flags).
- **`File stream overflow by command: none` but `RO` (reassembly overflow count) above is still
  > 0**: a real discrepancy - the per-command tracking missed something the plain counter saw.
  Worth a comment on #33 rather than trusting either number blindly; likely a bug in this PR's
  bookkeeping, not a hardware fluke.
- **Every per-command `max_wire`/`max_inflated` line this run is comfortably under
  `kReassemblyCapacity`/`kMaxInflatedSize`, and `File stream overflow by command: none`**: the
  31/2850 result from the `0d1edcc` hardware run didn't reproduce on this build. Worth noting
  explicitly either way (don't assume it was fixed by this PR - this PR doesn't change
  `kReassemblyCapacity`, so a disappearance would be a hardware/timing difference between runs,
  not this change).
- **`Inflate failed for command ... true inflated size NNNN`**: that command's real decompressed
  size, even though decompression itself failed - directly feeds `kMaxInflatedSize`'s step-2
  sizing the same way an overflow's `max_true_size` feeds `kReassemblyCapacity`'s.
- **No `Inflate failed` line at all this run**: either nothing this run needed decompression
  past `kMaxInflatedSize`, or (less likely, given the `f0a4939` result) that result doesn't
  reproduce - note which, don't assume silence means fixed.

**Not verified on real hardware** - this PR adds measurement only; no hardware run has been
done against this build. The `0d1edcc`/`f0a4939` numbers quoted above are from the owner's
prior runs, documented in issue #33 and the #60/#61 section above, not reproduced here.

**Hardware result (commit `872c898`): MPU fault, root cause found and fixed.** The owner's
hardware run on that commit halted with an MPU fault (Instruction Access Violation) on the
`usbh` thread about 4 seconds after the handshake, mid-`File`-stream - right around when the
first overflowing message would arrive. The faulting PC wasn't a valid code address, and the
saved LR (`0x1800d2f5`) resolved via `arm-zephyr-eabi-addr2line` against a from-source
`frdm_rw612` rebuild of `872c898` to `HidReassembler::appendTail()` (`HidReassembler.hpp:178`)
- confirming a stack smash rather than a logic bug. `appendTail()`'s stack `scratch` buffer
was sized for a chunk of at most `kHidMaxPayloadPerReport` (126) bytes, on the assumption
that's all a caller would ever pass - true for the per-report `chunk` argument, but not for
the overflow-transition call in `feed()`, which passes the *entire* accumulated `buffer_`
(up to `Capacity`, 4096 in `QcHidBridge.hpp`) in one shot once a message both overflows and
has already buffered more than one report's worth of bytes. Writing that into a
134-byte (`kTrailerTailSize + kHidMaxPayloadPerReport`) stack array smashed the stack and
corrupted the return address, producing exactly this symptom. Fixed by rewriting
`appendTail()` to need only `kTrailerTailSize` (8) bytes of stack regardless of its argument's
length - it only ever needs the last 8 bytes of (old tail ++ chunk), so a chunk already that
long or longer makes the old tail irrelevant instead of needing to hold both at once. Added
`test_overflow_with_large_buffered_body_before_transition` to `tests/hid_framing` (now 11
cases), reproducing the same shape at `Capacity=300`: two full 126-byte reports (252 buffered
bytes) land normally, so the third report's overflow-transition call passes a 252-byte
`buffer_` into `appendTail()` - already well past the old 134-byte scratch buffer. All 6
Ztest suites still pass (86/86, one new) on `qemu_cortex_m3`, and a clean `frdm_rw612` build
succeeds with unchanged RAM (97816 B, 9.95%). **Not verified on real hardware** - commit to
flash for that is the fix on top of `872c898`, still branch `firmware/reassembly-memory-budget`.

**Hardware measurement (commit `a1c25b6`): no crash, full by-command breakdown.** Handshake
`4.1.1`. `File` stream:

```
File stream finished (quiet): 551 messages (0 compressed, 0 failed), ... ms
File stream tally: command=4 count=551 max_wire=3946 ...
File stream overflow by command: command=4 count=29 max_true_size=113180
File stream overflow by command: command=51 count=1 max_true_size=59219
File stream overflow by command: command=35 count=1 max_true_size=28354
```

551 `File` messages fit under `kReassemblyCapacity` (largest 3946 bytes, none compressed), and
29 overflowed it - the largest of those 113180 bytes. `ModelRepo` (51, 59219 bytes) and
`ModuleStats` (35, 28354 bytes) each overflowed once. A compressed `RecallPreset` (15) later
inflated to 22667 bytes for one preset - past `kMaxInflatedSize` (16384) - a second preset
that same run inflated to 13235 bytes, under the limit. `malformed=0`, confirming the
`872c898`/fix-commit theory above: once overflow no longer desyncs the reassembler, spurious
`Malformed` counts disappear entirely. **The 29 lost `File` messages' contents are unknown** -
HidReassembler drops an overflowing message's body, keeping only its trailer (see
`overflowTrailer()` above), so whether any of them were preset folders - and therefore whether
this stream's preset list is missing folders - can't be answered from this run alone. That
question is exactly what the variant experiment below exists to answer.

## FileMessage READ options experiment, step 1.5 (issue #33)

Before sizing `kReassemblyCapacity`/`kMaxInflatedSize` for real, the owner asked for one more
measurement: does `FileMessage` READ's `short_listing` (field 6) or `omit_factory_content`
(field 11) option shrink the device's listing enough to avoid the overflow above - and if so,
does the shrunken listing still name every *user* preset? A third field,
`user_content_estimate` (field 12), exists on the same message but wasn't tested (it's an
estimate the device reports, not a request option - setting it on a READ has no documented
effect). All three field numbers were verified directly against qc-mcp's own descriptor set
(`qc_descriptors-4.1.pb`, field-by-field, same method issue #5's Tier 2 messages used), not
guessed - see `FileReadOptions` in
[`QcFileRequest.hpp`](app/src/QcFileRequest.hpp). **Nobody knows what either option does**:
neither qc-mcp's protocol code nor its CLI/MCP tool surface ever sets either field (checked
directly against its source), so this was genuinely untested territory until a real device
answered it below.

[`QcFileReadExperiment.hpp`](app/src/QcFileReadExperiment.hpp) is a **self-contained, isolated
diagnostic** - not a permanent feature, and not step 2's actual sizing pass. It's meant to be
deleted (the file itself, plus its few call sites in `QcSession.hpp` - grep `issue #33 variant
experiment`) once the owner has an answer. The two buffer sizes it temporarily raises
(`kReassemblyCapacity`, `QcDispatcher`'s mailbox - see "Buffer sizes" below) are also meant to
be revisited, not kept as-is; `kMaxInflatedSize` is untouched throughout.

### The test

After the handshake and issue #61's twelve post-connect state READs, instead of one bare
`File` READ this build sends four variants, one at a time, each to its own stream finish plus
a drain gap (see "Separating variants cleanly" below) before the next is sent - so a reply to
one variant is never counted against another:

1. **`short_listing=true`**
2. **`omit_factory_content=true`**
3. **both set**
4. **default** - both options false (byte-identical to the original bare READ; must reproduce
   the `a1c25b6` numbers above, re-confirming them under this run's conditions).

`default` moved to last deliberately, after a first hardware run (below) sending it first found
its own reply still trickling in when the next variant's request went out - ordering it last
means any contamination lands on the one variant whose real numbers are already known, not on
one this experiment is actually trying to measure.

Flash this build once; all four variants run automatically in one connect, back to back. A
variant that gets no reply, or a shorter stream than expected, is reported as such (a
`0`-message summary, same as any other quiet/timeout finish) and the sequence still moves on to
the next variant rather than stopping.

### Separating variants cleanly

A first hardware run (commit `147fb48`, "Hardware result 1" below) sent each next variant the
moment `QcFileStream`'s 1500ms quiet window reported the current one finished, and found that
too short: a genuine pause mid-reply looked like the reply ending, so the real tail arrived
*after* the next variant's request had already gone out and got counted against the wrong
variant. Two fixes, on top of each other:

- **`QcFileStream`'s own quiet window is now longer for this experiment** - 5000ms
  (`QcSession::kExperimentQuietWindowMs`), via a new optional parameter on `start()` (default
  argument keeps every existing caller/test unchanged). Long enough that a mid-reply pause
  doesn't look like the end.
- **A second gap between variants** - `QcSession::kVariantDrainGapMs`, 10000ms, checked every
  heartbeat tick (`checkDrainAndSendNextVariant()`) against `lastFileMessageAtMs_`, a timestamp
  updated on *any* `File` traffic, foldered or not, whether or not a stream is currently active.
  The next variant's request isn't sent until 10 full seconds of real silence have passed since
  the *last* `File` message of any kind - not just since `QcFileStream` itself called the
  current variant finished. A straggler arriving mid-gap resets the 10s countdown.

Also on the first hardware run, a single 14-byte reply with **no `type` and no `folder` field**
arrived alone for `short_listing=true`, 1552ms after the request, and `QcFileStream` read that
as the whole reply. **What this reply actually is isn't confirmed** - qc-mcp never sets
`short_listing`, so there's no existing code path that explains its shape; it might be an
estimate/ack the device sends before a `short_listing` stream specifically (consistent with
`user_content_estimate`, field 12, being device-reported rather than request-set - a guess, not
a confirmed reading of the bytes). Whatever it is, it isn't a catalog entry, so
`QcFileReadExperiment::hasFolder()` now gates it out: only a message that actually carries a
`folder` field feeds `QcFileStream`'s quiet-window tracking at all (see `QcSession::onMessage()`'s
comment). It still updates `lastFileMessageAtMs_` for the drain gap above, and still shows up in
the per-variant type tally as `type=-1`, so it stays visible rather than silently vanishing.

### Buffer sizes (temporary, for this measurement only)

Hardware result 1 found a `short_listing=true` message up to at least 10193 bytes true size -
over the 4096-byte `kReassemblyCapacity`/dispatcher-mailbox limits, so it was dropped before it
could ever be decoded. For this experiment only, both are temporarily raised to 16384 bytes:

- `QcHidBridge.hpp`'s `kReassemblyCapacity`: 4096 -> 16384.
- `QcDispatcher.hpp`'s `kMailboxCapacity`: 4096 -> 16384 (mirrors the above - a message's
  protobuf payload can never be bigger than the whole reassembled HID message it came from).

`QcGzip.hpp`'s `kMaxCompressedInputSize`/`kMaxInflatedSize` are **not** touched: every hardware
run so far shows `File` messages are never gzip-compressed, so gzip's own buffers aren't on the
path this needs to grow, and `kMaxInflatedSize` is explicitly out of scope for this issue
regardless. **This is not #33's final answer** - it's sized to let `short_listing`'s messages
through so they can be measured at all, not a considered sizing choice; step 2 still needs to
pick real numbers from whatever this run shows (see "RAM cost" below for what carrying 16 KiB
costs today).

### What gets logged per variant

Each line tagged `[variant=<name>]` so the four runs can't be confused with each other:

```
File stream [variant=<name>] finished (quiet|timeout): N messages (C compressed, F failed), W wire bytes, I inflated bytes, E ms
File stream [variant=<name>] limits: max wire=MW max inflated=MI, dropped (dispatch=DD gzip=DG), reassembly (overflow=RO malformed=RM)
File stream [variant=<name>] tally: T total IN transfers
File stream [variant=<name>] tally: command=CMD count=CNT max_wire=... max_inflated=...   (per distinct command seen)
File stream [variant=<name>] overflow by command: command=CMD count=CNT max_true_size=MTS   (or "none")
File content [variant=<name>]: preset folders=PF (factory=FF user=UF) named presets=NP (factory=FP user=UP) empty slots=ES
File content [variant=<name>]: type=T count=C   (per distinct File `type` value seen, or "no File messages of any type seen this variant")
```

`overflow by command` is **reset at the start of each variant** (`QcReassemblyOverflowTally::reset()`),
so it reports only that variant's overflows, not a running lifetime total - unlike the single-run
measurement earlier in this section.

The **content** line's counts come from actually decoding each fitting message with
[`QcFileMessage.hpp`](app/src/QcFileMessage.hpp)'s `decodeFile()` - the same merged decoder
issue #59 wrote and issue #10 will eventually wire into the real session - called as each
message arrives, off `usbh`, on whichever worker thread (`qc_dispatch`/`qc_gzip_inflate`)
dispatched it; see `QcFileReadExperiment::onFileMessage()`'s own comment for why that's
stack-safe. **"Named" excludes empty-slot entries**: a blank preset name is an unused slot, not
a preset - confirmed on real hardware (2026-11-01, see "Hardware result 2" below: the owner's
device really does have exactly two named presets in a 256-entry folder, matching 254 reported
as empty) - same pattern qc-mcp's `directory.py` documents for `SetlistPosition`'s own
fixed-size table. `factory`/`user` splits use each folder's `is_factory` flag. The `type=` lines
come from reading each message's own `type` field directly, independent of `decodeFile()`'s
presets-only filtering, so an IRs/Captures folder, or the no-folder reply discussed above
(`type=-1`), still shows up somewhere even though `decodeFile()` itself never calls back for
either.

**No live per-message logging during the stream anymore** - see "Dispatcher drops" below for
why. Everything above is only *accumulated* while the stream runs and printed afterward, from
the heartbeat thread, once the whole variant has gone quiet.

### Per-folder detail for the `short_listing` variants

For the two variants with `short_listing=true` (1 and 3 above), the catalog is expected to
shrink to a handful of folders (confirmed on hardware: 13), small enough to dump in full rather
than just totals:

```
File content [variant=<name>] folder: key="..." name="..." parent_key="..." is_factory=0/1 is_downloads=0/1 named=N empty=E
File content [variant=<name>] folder "<name>" sample: "<preset name>"   (up to 3 per folder, named presets only)
```

`QcFileReadExperiment::FolderDetail` holds up to `kMaxFolders` (16) folders' key/name/
parent_key/is_factory/is_downloads/named-and-empty counts/up to 3 sample names - sized for the
13 folders `short_listing` actually produces, not for `default`/`omit_factory_content` alone's
few hundred (confirmed: 376/390), for which only the totals above are logged. A folder key seen
again replaces that slot's content rather than accumulating into it, matching qc-mcp's own
`directory.py` comment ("later messages for the same folder key replace earlier ones"). This
table is populated for every variant (cheap, and harmless for the ones that overflow it into
`otherFolderCount`) but only *printed* for the short-listing ones.

### What each outcome means

- **A variant has zero `File` overflows (`overflow by command: none`, and every per-command
  `max_wire` comfortably under `kReassemblyCapacity`) *and* its named-preset/folder counts match
  (or exceed) `default`'s, with no new empty slots** - this is **success**: that option (or
  combination) makes the whole catalog fit without losing anything, and step 2 can size
  `kReassemblyCapacity` to that variant's real peak instead of buffering (or excluding) a
  message as large as `a1c25b6`'s 113180-byte one.
- **A variant has fewer messages or fewer overflows than `default`, but also fewer named
  presets, fewer preset folders, or a folder present in `default` missing by name from the
  short-listing dump - this is *not* useful**, even though it looks smaller: the option is
  dropping user content, not just shrinking factory bulk. Say so plainly rather than treating a
  smaller number as automatically better - see "Hardware result 2" below for a concrete example
  of checking this (`My Presets`).
- **`omit_factory_content=true` loses exactly the factory presets/folders** (check surviving
  folders' `is_factory` against `default`'s own folders, not just a smaller total) **but keeps
  every user folder and preset name intact** - also success, and the more useful of the two
  single-option variants if so, since factory content is the device's own bulk, not something a
  user would notice missing from a *listing* request (it's still on the device either way).
- **`short_listing=true` shrinks messages but strips preset names** (named-preset count drops
  relative to `default`, or every sampled name is a real one but whole user folders are simply
  absent from the dump) - not useful on its own.
- **Neither option changes anything measurable** (same message count, same overflows, same
  content, byte-for-byte) - a real, useful negative result: it means qc-mcp's own choice to
  never set these fields isn't an oversight, and step 2 has to size for the full, unfiltered
  catalog (or explicitly exclude very large messages like `ModelRepo`, per the issue's own
  framing) rather than relying on a request-side shortcut.
- **A variant's stream never starts** (0 messages, 0 total IN transfers, times out at 30s) -
  worth a comment on #33 before assuming the option itself broke something.

### Hardware result 1 (commit `147fb48`): no crash, but variants overlapped

Handshake `4.1.1`, no crash (the stack-overflow fix below held). Summarized (full detail in the
issue):

- **default** (sent first that run): 547 `File` counted, `dispatcherDropped=4`, overflow=29
  (max 113180), 376 preset folders, 2810 presets, 254 empty-named.
- **`short_listing=true`**: 1 `File` message, 14 bytes, type=-1, finished in 1552ms.
- **`omit_factory_content=true`**: 565 `File`, dropped=3, overflow=32 (max 113181), 390 folders,
  3071 presets, 508 empty-named, 9106ms.
- **both**: 18 `File`, 6625 wire bytes, max 2230, overflow=3 (max 10193), 13 preset folders, 256
  presets, 254 empty-named.
- Samples: "Practice 1" (folder "Practice"), then empty names in "Practice"; "The Pop Rig" in
  "02 Session Sounds"; "Shy Lead" in "Jack Gardiner".

**Diagnosis - variant 4 ("both") was contaminated**: 565 = 547 + 18, overflow 32 = 29 + 3,
empty-named 508 = 254 x 2. `short_listing`'s real reply almost certainly arrived *after* the
1500ms quiet window had already closed its variant, and got counted into the next one
(`omit_factory_content`'s 565/32/508 each look exactly like `default`'s own number plus
`short_listing`'s). So **neither `short_listing` alone nor `omit_factory_content` alone was
actually proven by this run** - this is why "Separating variants cleanly" above exists, and why
this needs to be re-run.

**Dispatcher drops, confirmed cause.** `dispatcherDropped=4` was new - every prior `File` stream
measurement (including `a1c25b6`'s, on the exact same codepath otherwise) showed 0. 547 = 551
(`a1c25b6`'s own count) - 4, lining up exactly. The only change on the dispatcher thread's path
between those two runs was this experiment's `decodeFile()` call plus an inline `LOG_INF` for
each sample. `CONFIG_LOG_MODE_IMMEDIATE` runs a log call's `cbprintf` formatting - and the UART
write - synchronously on the calling thread; during that write, `QcDispatcher`'s single mailbox
slot can't accept the device's next message. The arithmetic lining up exactly is strong evidence
this is the cause, though it wasn't isolated further (e.g. by measuring `decodeFile()` alone
without logging) before fixing it. **Fix applied**: all per-message logging moved off the
dispatcher/gzip-inflate threads entirely - `onFileMessage()` now only accumulates (into the
per-variant totals and the per-folder table), with zero `LOG_*` calls; every log line for a
variant is produced afterward, from the heartbeat thread, once that variant's stream has gone
quiet. **Implication for issue #10**: `decodeFile()` itself appears cheap enough to run inline
on the dispatch path (0 drops before logging was added), but logging/forwarding decoded content
must not happen there - #10's real wiring should defer that to a separate thread or a batched
step, not the dispatcher's hot path. Whether drops are now fully gone, or `decodeFile()`'s own
cost was also a (smaller) contributor, isn't confirmed - the next hardware run's `dropped=`
figures will show which.

### Hardware result 2: ground truth from the owner

The owner's unit has exactly two user folders: `My Presets` and `Practice`. Checked against the
sampled/decoded names above:

- **Confirmed**: `Practice` holds exactly `Practice 1` and `Dark Jazz` - matching the "both"
  variant's decode exactly (256 presets, 254 empty leaves 2 named; every decoded preset in that
  run was `Practice`'s). The other 12 preset folders in that run decoded with zero presets each,
  or (more likely, given the contamination above) their content is among the 3 overflowed
  messages (up to 10193 bytes) that run never got to see with the old 4096-byte buffer - the
  16 KiB buffer above exists specifically so a re-run can.
  - The owner switched from `Practice 1` to `Dark Jazz` during this run, matching the single
    `RecallPreset` (1223 -> 13235 bytes) seen in the log - so `Dark Jazz` is ~13 KiB inflated.
    `Practice 1` is the larger of the two (more effects/rows per the owner) and is **inferred,
    not confirmed**, to be the 22667-byte preset from an earlier run.
- **Unconfirmed, worth checking on the next run**: `The Pop Rig` (folder "02 Session Sounds")
  and `Shy Lead` (folder "Jack Gardiner") aren't presets the owner recognizes - they may be
  factory or cloud-downloaded content rather than a decode error. The per-folder dump now
  reports `is_factory`/`is_downloads` for every folder specifically so this can be checked
  directly against those flags rather than guessed from the name.
- **Open question for the next run**: is `My Presets` visible by name, with its real presets,
  in the `short_listing` dump? Earlier hardware debugging saw recall positions 12 and 13 active
  in it, so it has at least 14 addressable slots - its real named-preset count isn't known yet.
  If `My Presets` is still missing even with the 16 KiB buffer and properly separated variants,
  that means `short_listing` is not usable as-is, and should be reported exactly that plainly.

### Hardware stack overflows: three so far, all fixed

This experiment has now overflowed a thread's stack on real hardware three separate times -
each only found via a crash, then resolving the fault PC/LR with `arm-zephyr-eabi-addr2line`
against that commit's `zephyr.elf`, the same method issue #8's original `usbh` overflow used:

1. **`usbh`** (fixed before `147fb48`) - see the earlier `872c898` section above
   (`HidReassembler::appendTail()`'s unbounded scratch buffer).
2. **`qc_heartbeat`** (fixed before `147fb48`) - USAGE FAULT, "Stack overflow, context area not
   valid", on its very first tick, before the handshake even finished. Cause: the per-variant
   logging and next-variant-send logic was inline in `runHeartbeat()` itself; at this build's
   optimization level, a function's stack frame reserves space for every local declared
   anywhere in its body, including inside a branch that's almost never taken (`if (summary)`) -
   so `qc_heartbeat`'s 2048-byte stack (measured peak 1052 B on `a1c25b6`, before this
   experiment existed) was oversubscribed on *every* tick, not just the few where that code
   actually ran. Fixed by moving that logic into its own method,
   [`logFileStreamSummaryAndAdvance()`](app/src/QcSession.hpp) - its locals are now a separate
   call frame, paid for only while that function actually executes. `heartbeatStack_` was also
   doubled to 4096 B for margin.
3. **`qc_session`** (fixed in this commit) - same fault signature, on `f803e75`, immediately
   after `Session handshake complete`, before the first variant's own log line. `addr2line`
   against `f803e75`'s `zephyr.elf` resolved the fault PC/LR to
   [`QcFileReadExperiment::beginVariant()`](app/src/QcFileReadExperiment.hpp), called from
   `QcSession::runHandshake()`. Cause: `beginVariant()` reset the per-folder table with
   `folders_ = {}` - assigning a freshly-constructed `std::array<FolderDetail, kMaxFolders>` (16
   non-trivial objects, each holding several `etl::string` members, roughly 7 KiB total). At
   this build's optimization level that materializes the whole temporary on the calling thread's
   stack before the assignment completes - the same class of bug as the two faults above, just a
   third distinct place it showed up. `qc_session`'s 2048-byte stack (used for the one-time
   handshake, never bumped because nothing on it looked expensive) had no margin for a 7 KiB
   spike. **Fixed by not resetting `folders_` itself at all**: `folderCount_ = 0` is sufficient on
   its own (every reader is already bounded by `folderCount_`, so stale bytes past it are never
   read), and `fillFolder()` - called for every slot `recordFolder()` claims, new or reused - now
   resets a slot's counts/sample-count along with its metadata, so a slot index reused from an
   earlier variant can't leak that variant's stale counts into the new one. This also fixed a
   related correctness bug: the "claim a brand-new slot" path previously never reset
   `namedCount`/`emptyCount`/`sampleCount` at all (only the "found an existing key" path did),
   so a slot reused across variants could have silently carried over a previous variant's counts
   even before this crash surfaced the issue.

**Stack-usage audit, after fix 3** (`-fstack-usage`, `Main.cpp.su`, worst sequential call chain
per thread - `logFileStreamSummaryAndAdvance()`/`checkDrainAndSendNextVariant()` run one at a
time per heartbeat tick, never nested, so their frames don't add together):

| Thread | Entry point | Budget | Worst-case depth found | Margin |
| --- | --- | --- | --- | --- |
| `usbh` | `onReportIn()` -> `appendTail()` | 1536 B | ~104 B (unchanged this round; real hardware previously measured 736 B peak) | ~93% free |
| `qc_session` | `runHandshake()` -> `sendMessage()`/`encodeFileRead()` | 2048 B | ~640 B | ~69% free |
| `qc_heartbeat` | `runHeartbeat()` -> `logFileStreamSummaryAndAdvance()` | 4096 B | ~1608 B | ~61% free |
| `qc_dispatch` | `onMessage()` -> `onFileMessage()` -> `peekFolderMeta()` -> `ProtoFieldReader::next()` | 2048 B | ~760 B | ~63% free |
| `qc_gzip_inflate` | `run()` -> `onMessage()` -> ... (same tail as `qc_dispatch`) | 4096 B | ~848 B | ~79% free |

`runHeartbeat()`'s own frame (1136 B, the single largest in the app) is almost entirely two
pre-existing `std::array<QcMessageTally::Entry, 24>` locals (`tallyEntries`/`postStreamEntries`)
that predate this experiment (issue #60/#61) - not a new regression, and still comfortably
within the doubled 4096-byte budget. Nothing else found needed a fix; the table above is a
point-in-time static check, not a replacement for a real hardware run (compiler inlining
decisions, and anything genuinely data-dependent, can't be fully captured this way).

**`CONFIG_THREAD_ANALYZER` is now on** for this diagnostic build (`CONFIG_THREAD_ANALYZER_AUTO`,
printing every thread's real stack high-water mark every 15s) specifically so a tight margin
shows up in the log on the very first run, rather than needing a fourth crash-and-addr2line
round trip to find one. Revert alongside the rest of this experiment.

### RAM cost

The 16 KiB reassembly/dispatcher buffers (temporary, see "Buffer sizes" above) and
`QcFileReadExperiment`'s per-folder table are each one static instance - not stack, so they add
to the firmware's flat RAM footprint, not any thread's margin:

- `kReassemblyCapacity`/dispatcher mailbox, 4096 -> 16384 each: +12288 B apiece (+24576 B total).
- `QcFileReadExperiment::folders_` (16 x `FolderDetail`, each three `etl::string<64>` plus three
  `etl::string<48>` samples and a handful of counters): roughly +7 KiB.
- `CONFIG_THREAD_ANALYZER`'s own bookkeeping plus its periodic analysis thread's stack: a few KB.

Clean `frdm_rw612` build: RAM 134776/983040 bytes (**13.71%**, up from `a1c25b6`'s 97816 B /
9.95% - the roughly 37 KiB above, comfortably inside this board's 960 KiB). None of this is
step 2's final sizing; it's what this measurement run costs.

### Verified

`west twister -p qemu_cortex_m3 -K`, all 6 suites, 101/101 cases pass (15 new over `a1c25b6`:
the reordered variant table, the no-folder/`hasFolder()` path, per-folder detail capture/replace/
bounding, `QcFileStream`'s overridable quiet window, and the existing `FileReadOptions`
encoding tests - all still pass after this commit's `fillFolder()`/`beginVariant()` fix). Clean
`frdm_rw612` build succeeds (RAM above). **Not verified on real hardware** - this commit's own
variant-separation/ack-handling/buffer-size/logging/stack-overflow changes have not yet had a
hardware run; commit to flash for that is this PR's tip, still branch
`firmware/reassembly-memory-budget`.

## Not yet done

- ETL is wired in via CMake `FetchContent` (see `app/CMakeLists.txt`) but not yet used by
  the main app - only by the HID framing tests above.
- The KeepAlive heartbeat (issue #8, see above) is implemented, unit-tested, and verified
  on real hardware: the cleaned-up build (`f8ad423`) passed its smoke test (boot, handshake
  mirrors `4.1.1`, no fallback warning, no crash over a couple of minutes), and the earlier
  diagnostics-carrying build (`584e015`) showed repeated reads answered for several minutes
  under the heartbeat. A heartbeat-off comparison is still unconfirmed. Issue #61's state READs
  and preset-change push are now confirmed on real hardware too (commit `0d1edcc`, see "Hardware
  results" in the #60/#61 section above) - a compressed `RecallPreset` arrived with the owner's
  preset change and inflated correctly.
- Tier 2's `decodeModelRepo`/`decodeSetlistPosition`/`decodeFile` aren't called from anywhere
  yet - wiring them into the session/message-dispatch path is for whichever issue actually
  consumes them (`ModelRepo` needs #6's inflate first; a preset-list `SetlistPosition` READ and
  the `File` request/dispatch are issue #10's scope).
