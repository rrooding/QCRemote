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
- **No preset-change push was observed** (`RecallPreset`/`Scene`/`SetlistPosition`/
  `PresetDirty`/`RecentsFavorites`, per `PROTOCOL.md`'s description of what a device-side
  preset recall autonomously pushes). One candidate explanation, not a proven one: qc-mcp's
  own `transport.py::_handshake()` sends twelve explicit subscription READs (including
  `SetlistPosition`/`PresetDirty`) right after `Connection` UPDATE, with a comment that this
  is what "makes the QC start streaming" — this firmware has never sent any of those.
  Sending them is issue **#9**'s scope (and #5 Tier 2's), not this one's. If presets are
  still silent once #9 lands, that candidate explanation needs revisiting.

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
`Version` reply, and no File traffic at all) made these the leading candidate explanation, though
still not a proven one until a real run with them in place says otherwise either way.

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
  processed, or one too big for the 4096-byte mailbox, is dropped. Both now count every drop
  (`droppedCount()`, a lifetime total); `QcSession` snapshots that count when the File stream
  starts and diffs it at the end, so the summary's `dispatcherDropped`/`gzipDropped` report only
  what was lost *during this stream*. **There's no hardware measurement yet of whether one slot
  actually keeps up with the device's real arrival rate** - that's exactly what these fields are
  for: 0 across a real run means the assumption holds; consistently above 0 is real evidence of
  data loss by design, and the fix would be a small bounded queue - not something worth adding
  speculatively ahead of that evidence.
- A File folder message that overflows `QcHidBridge`'s `kReassemblyCapacity` (4096 bytes) is
  dropped by `HidReassembler` *before* `handleMessage()` ever sees a command id, so - unlike the
  two drops above - it can't be attributed to the File stream specifically; the same goes for a
  malformed HID report. Both are still counted (`reassemblyOverflowCount`/
  `reassemblyMalformedCount`, same snapshot-and-diff treatment) and folded into the summary as a
  best-effort "this happened somewhere during the stream" figure, rather than left solely to a
  log line. These used to log via `LOG_WRN` directly in `QcHidBridge::onReportIn()` - run on the
  `usbh` thread, where (unlike the already-filtered-out-by-default `LOG_DBG` calls nearby) an
  *enabled* level like `LOG_WRN` really does run `cbprintf` formatting synchronously on that
  1536-byte stack, the same pattern that overflowed it once before (see issue #8's section
  above). Switched to atomic counters instead, with no logging on that thread at all for these
  two cases now.

A File message that decodes far enough to detect gzip but then fails to inflate (e.g. exceeds
`QcGzip.hpp`'s `kMaxInflatedSize`, 16384 bytes) is counted differently again - via
`onInflateFailed()`/`failedCount`, since by that point a command id and a wire size *are* known.
All of the 4096/16384-byte limits above are issue #33's placeholders to size for real, not
something this issue changes; the summary's new `maxWireBytes`/`maxInflatedBytes` fields (the
largest single message actually seen, successfully processed - a message dropped for being too
big never reaches this tracking at all) are what #33 needs for that sizing pass.

**Not yet verified on real hardware.** Two hardware runs exist for issue #60's File stream itself
(both predate #61's state READs - see the `11e43d0` result a few paragraphs up and the outcome
list below), and neither saw File traffic. What's not yet verified on hardware at all: whether
#61's state READs change that result, and everything about the post-connect tally/timed
preset-change step below, all new in this PR. Verify with:

1. Flash this build and open the serial console (see Flash/Verify above).
2. Confirm the handshake completes as before (`Session handshake complete (firmware version:
   ...)`, no `No usable Version reply` warning).
3. Capture continuously for at least 35 seconds after that line (long enough to cover the 30s
   overall cap even if the stream never goes quiet).
4. Paste back the full captured output from boot through either the `File stream finished (...)`
   line or the 35s mark, whichever comes first.
5. **Timed preset-change step (issue #61):** once the `File stream finished (...)`/`File stream
   tally: ...` lines have appeared, change the loaded preset once on the unit itself (not through
   this firmware - there's no control path for that yet), note the wall-clock time you did it, and
   keep capturing for another 30 seconds. Paste back everything from the `File stream tally:
   command=...` lines (or the "no messages" line) through that 30-second mark, and say when (which
   log line, or roughly how many seconds in) the preset change happened relative to the capture.
   This is what tells us whether a preset recall on the unit pushes anything
   (`RecallPreset`/`Scene`/`SetlistPosition`/`PresetDirty`/similar) - see `Post-connect tally`
   below for where that shows up.

A finished stream now logs several lines - paste back all of them:

```
File stream finished (quiet): N messages (C compressed, F failed), W wire bytes, I inflated bytes, E ms
File stream limits: max wire=MW max inflated=MI, dropped (dispatch=DD gzip=DG), reassembly (overflow=RO malformed=RM)
File stream tally: T total IN transfers
File stream tally: command=CMD count=CNT    (one line per distinct command id seen, or "no messages of any command seen in this window")
Post-connect tally started: watching for preset-change pushes (RecallPreset/Scene/SetlistPosition/...) - issue #61
```

The middle two are a diagnostic added after the first hardware run reported a File stream that
finished with every count above at 0 (`0 messages ... 0 wire bytes ... 0 inflated bytes`, dropped
and reassembly all 0 too) - evidence consistent with either "the device sent nothing at all
during the window" or "the device sent something, just never anything this build recognized as
File" (every other command's traffic was invisible before this). `QcMessageTally.hpp`
(`QcSession`'s `messageTally_`) records every message handed to `onMessage()`/`onInflateFailed()`
by command id, not just File, for the lifetime of one File stream; `T` (`QcSession::totalInTransfers_`,
incremented in `QcHidBridge::onReportIn()`) counts one layer lower still - every successful USB IN
transfer, regardless of whether the reassembler ever turned it into a counted message at all. The
per-transfer `LOG_INF` in `usbh_shim.c` that used to show this detail report-by-report is now
`LOG_DBG` (and that module registers at `LOG_LEVEL_INF`, so it compiles out entirely) - `T` is the
one place this count still appears, without swamping the log or running extra formatting on the
`usbh` thread.

The last line starts a second, separate tally (`QcSession`'s `postStreamTally_`, same
`QcMessageTally.hpp` class, a different instance) that keeps running for the rest of the session,
specifically for issue #61's preset-change check: `messageTally_` above stops dead the moment the
File stream finishes, so without this there'd be no visibility into anything the device sends
afterward - including a preset-change push. Once started, it logs a fresh
`Post-connect tally: command=CMD count=CNT` line (same per-command shape as the File stream's own
tally lines, plus a `Post-connect tally: N messages from other distinct commands (> 8 tracked)`
line if more than 8 distinct commands show up) whenever something changes - immediately for a
command id never seen before in this window (the actual preset-change signal), throttled to at
most once per second for a plain count bump on an already-seen command (so continuous streaming
traffic alone - qc-mcp's own comment: GlobalTempo/CPULoad at ~3/s under an active heartbeat, with
or without a preset change - can't flood the log for the rest of the session). If nothing at all
shows up after the stream, you'll simply see no further `Post-connect tally: ...` lines once the
one above.

What each outcome means:

- **`File stream finished (quiet): ...`**, with `E` landing somewhere around 10-15 seconds and `N`
  in the neighborhood of qc-mcp's ~400: the whole path works end to end - File READ reached the
  device, the stream was collected, and the quiet window correctly ended it. `C`/`F`/the size
  totals are the numbers issue #33 needs.
- **`File stream finished (timeout): ...`**: the stream never went quiet inside 30s - either the
  device kept sending (worth a longer capture to see if it ever stops) or something is stuck
  resetting the quiet window without the stream actually making progress.
- **`DD`/`DG` (dispatcherDropped/gzipDropped) above 0**: real evidence that the single mailbox
  slot in `QcDispatcher`/`QcGzipInflater` couldn't keep up with the device's arrival rate during
  this stream - messages were dropped, not just slow. If this is 0 across a full run, the
  single-slot design is fine as-is at this device's actual rate; if it's consistently above 0
  on repeat runs, that's the signal to add a small bounded queue (not something to do without
  this evidence). Either way, `N` above is then a *lower bound* on how many File messages the
  device actually sent, not the true count.
- **`RO`/`RM` (reassemblyOverflowCount/reassemblyMalformedCount) above 0**: a File folder message
  overflowed `kReassemblyCapacity` (4096 bytes) or an HID report was malformed, during this
  stream's window - the known gap: these happen before a command id is known, so they can't be
  confirmed to be File messages specifically, but during this stream, File traffic is almost
  everything on the wire. These no longer log their own warning (switched to counters - see
  above), so this line is the only place they show up now.
- **`MW`/`MI` (maxWireBytes/maxInflatedBytes)**: the largest single File message actually seen.
  If `DD`/`DG`/`RO` are all 0, these are the true peak and exactly what issue #33 needs to size
  `kReassemblyCapacity`/`kMaxInflatedSize` against. If any of those are above 0, the true peak is
  *at least* this large - some messages that would have set a new peak never made it far enough
  to be measured.
- **No `File stream finished` line at all within 35s, and no `Handshake: failed to send File
  READ` error either**: the request went out but nothing about it is recognizable as a File
  reply - check whether `RO`/`RM` end up nonzero once a summary does eventually land, or whether
  the eventual message count is unexpectedly low.
- **A `File stream finished (timeout)` with every count (`N`, `W`, `I`, `DD`, `DG`, `RO`, `RM`)
  at 0, and `T` (total IN transfers) also 0**: nothing arrived on the wire at all during the
  window - not a counting gap in this firmware, a silent device/USB layer.
- **The same all-zero summary, but `T` > 0**: the device answered with *something* at the USB
  level, so the session isn't completely inert - check `File stream tally`'s per-command lines
  (or "no messages of any command seen", if the tally itself is empty despite `T` > 0, which
  would point at `RO`/`RM` instead - a reassembly-level drop, not a session-activity one) to see
  what command(s) it actually sent. A nonzero `T` with an empty tally and `RO`/`RM` still at 0 at
  the same time shouldn't happen (every successful transfer feeds the reassembler, which must
  land on Complete/Overflow/Malformed/InProgress) - treat that combination as a bug in the
  counting itself, not a device behavior, and say so on the issue rather than guessing further.
- **A `File stream tally` with other commands present but no command 4 (File)**: the device is
  live and talking, just never answered the File READ specifically - that's a File-specific gap
  (worth comparing the exact READ bytes against qc-mcp's `list_directory()`: command id 4,
  `action=READ`, a `request_id` field - `QcFileRequest.hpp`'s `encodeFileRead()` already matches
  all three). **This is exactly what the second hardware run (commit `11e43d0`, after the
  `T`/tally diagnostics landed but before issue #61's state READs) showed**: `File stream
  finished (timeout): 0 messages ... 30138 ms` with every drop/overflow/malformed count at 0, but
  `File stream tally: 1 total IN transfers` and `File stream tally: command=10 count=1` - exactly
  one message the whole 30 seconds, and it was `Version` (command 10, almost certainly the
  handshake's own delayed reply lingering in the same window), not File. That's a live-but-silent
  session: the device accepted the connection and answered one thing, but sent nothing else at
  all for 30 seconds, including no File reply. qc-mcp's own comment that its twelve state READs
  are what "makes the QC start streaming and marks the session active" made them the leading
  candidate explanation for this specific result - **unproven, not confirmed**: this PR adds
  those twelve READs (see above) on exactly that theory, but whether they actually change this
  outcome is still an open question for the next hardware run, not a settled one. If the next run
  still shows a `T`/tally this sparse with the state READs now in place, that candidate
  explanation should be treated as ruled out, not just unproven, and the search should move
  elsewhere (re-checking the handshake sequence itself, or whether the device needs something
  this firmware isn't sending at all yet).
- **A `Post-connect tally: command=CMD count=CNT` line appears after the stream** (beyond the
  one-time `Post-connect tally started: ...` line): something arrived post-stream. If `CMD` is
  `RecallPreset` (15), `Scene` (13), `SetlistPosition` (2), `PresetDirty` (34), or
  `RecentsFavorites` (20), and it lines up with when you changed the preset in the timed
  preset-change step above, that's the preset-change push issue #61 asks about, finally observed
  - say which command(s) appeared and roughly how long after the preset change. If `GlobalTempo`
  (33) or `CPULoad` (26) show up continuously even *before* any preset change, that's qc-mcp's
  own documented behavior (streaming under an active heartbeat) rather than anything
  preset-specific - expected, not a new finding on its own.
- **No `Post-connect tally: ...` line at all after the one-time "started" line, through the end
  of the timed preset-change step**: the device stayed silent even after a preset change on the
  unit - record that plainly rather than assuming it; per the issue's own wording, if presets are
  still silent after #61 lands, that needs recording and investigating further, not treated as a
  bug in this PR's counting (the same generic dispatch path that counts File traffic counts this
  too, so a real push would show up here the same way).
- **`Handshake: failed to send File READ`**: the request itself never got sent - the handshake
  completed, so this would be a new, File-specific send failure rather than the already-explained
  benign `-EPIPE` quirk (`sendMessage()` only returns false when `sendReport` fails with
  something else). The same applies to `Handshake: failed to send state READ (command=...)` for
  one of issue #61's twelve READs - logged as a warning, not fatal, and doesn't stop the rest of
  the sequence or the File READ that follows (see the state-READs section above for why).
- **Occasional `File message failed to inflate (wire=... bytes): ...` warnings**: expected if a
  folder message's compressed payload decompresses past `kMaxInflatedSize` - counted in the
  summary's `failed` tally, not a crash and not supposed to stall later messages. The same applies
  to a `ModelRepo` reply specifically (issue #61) - it's handled by the same inflate path.
- **A crash or stack fault**: capture the fault PC/LR and `addr2line` against `zephyr.elf`, same
  method used to find issue #8's `usbh`-stack overflow (see that section above) - and say which
  thread's name appears in the dump (`qc_dispatch`, `qc_gzip_inflate`, `qc_session`,
  `qc_heartbeat`, or `usbh` itself).

## Not yet done

- ETL is wired in via CMake `FetchContent` (see `app/CMakeLists.txt`) but not yet used by
  the main app - only by the HID framing tests above.
- The KeepAlive heartbeat (issue #8, see above) is implemented, unit-tested, and verified
  on real hardware: the cleaned-up build (`f8ad423`) passed its smoke test (boot, handshake
  mirrors `4.1.1`, no fallback warning, no crash over a couple of minutes), and the earlier
  diagnostics-carrying build (`584e015`) showed repeated reads answered for several minutes
  under the heartbeat. A heartbeat-off comparison is still unconfirmed; the preset-change push is
  issue #61's scope (see above) - its state READs are implemented and unit-tested here, but
  whether they actually produce a push on real hardware is still unverified.
- Tier 2's `decodeModelRepo`/`decodeSetlistPosition`/`decodeFile` aren't called from anywhere
  yet - wiring them into the session/message-dispatch path is for whichever issue actually
  consumes them (`ModelRepo` needs #6's inflate first; a preset-list `SetlistPosition` READ and
  the `File` request/dispatch are issue #10's scope).
