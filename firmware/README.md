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
    │   └── HidChunker.hpp          # qc-mcp framing: a message in, reports out
    └── tests/
        └── hid_framing/            # Ztest suite for HidReassembler/HidChunker - no hardware needed
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
string if no reply arrives in time — qc-mcp's `transport.py::_handshake()` does the same,
with an even shorter 150ms window), and `Connection` UPDATE on a dedicated thread, per
PROTOCOL.md section 3. `qc_usbh_send_report()`'s `SET_REPORT` control transfer
(`usbh_shim.c`) is what actually gets the handshake messages onto the wire, since interface
5 has no interrupt OUT endpoint (see above).

**Verified 2026-10-31** against a real Quad Cortex Mini:

```
<inf> main: Claimed Quad Cortex Mini HID interface 5 (sub=0, proto=0)
<inf> usbh_shim: Found endpoint 0x81, attributes 0x03, wMaxPacketSize 128
<inf> usbh_shim: Interface 5 endpoints: IN=0x81
<inf> usbh_shim: Starting receive loop on IN endpoint 0x81
<inf> usbh_shim: IN transfer completed: 129 bytes
<inf> main: Reassembled message: 41 bytes
<wrn> main: No usable Version reply (gotReply=0), using fallback 4.1.0
<inf> main: Session handshake complete (firmware version: 4.1.0)
<inf> usbh_shim: IN transfer completed: 129 bytes
<inf> usbh_shim: IN transfer completed: 129 bytes
<inf> usbh_shim: IN transfer completed: 129 bytes
<inf> main: Reassembled message: 331 bytes
<inf> usbh_shim: IN transfer completed: 129 bytes
<inf> main: Reassembled message: 10 bytes
<inf> usbh_shim: IN transfer completed: 129 bytes
<inf> main: Reassembled message: 14 bytes
```

Right after "Session handshake complete", the device starts pushing data unprompted — the
"burst of state READs" PROTOCOL.md §3 step 4 describes (`ModelRepo`, `ModuleStats`, etc.),
which only happens once the device treats the session as live. That's the real confirmation
here, not just "no error was logged": the device's own behavior changed because it now
considers this a connected session, matching Cortex Control's own effect on it.

**Hardware surprise, now a documented and handled quirk**: `qc_usbh_send_report()`'s
`SET_REPORT` call returns `-EPIPE` (a stall) on *every* send to this device, including in
this trace, even though the handshake plainly works (see the burst above). This isn't a
parameter bug — confirmed by reading the device's actual HID report descriptor for the
first time (`05 01 09 00 a1 01 15 00 26 ff 00 85 01 75 08 95 80 09 00 81 82 85 02 75 08 95
80 09 00 91 82 c0`, decoded: Report ID 1 = 128-byte Input, Report ID 2 = 128-byte Output),
which matches `qc_usbh_send_report()`'s construction exactly (wValue `0x0202`, wLength 129).
`GET_REPORT` for both report IDs succeeds cleanly; only the OUT direction stalls, regardless
of data-phase encoding (tried both with and without the report id byte duplicated in the
data). It turns out this is already documented: PROTOCOL.md §1 — *"macOS quirk:
`IOHIDDeviceSetReport` returns a benign `0xe0005000` on this device — the official app
ignores it too, and the data is still sent. Not an error."* A stall on the control
endpoint's status stage self-clears on the next SETUP packet (unlike other endpoints, which
need an explicit `CLEAR_FEATURE`), consistent with the data having actually landed despite
the reported error. `QcSession::sendMessage()` now treats `-EPIPE` specifically as
non-fatal and continues, matching what every reference implementation already does.

## KeepAlive heartbeat (issue #8)

[`QcSession.hpp`](app/src/QcSession.hpp) now also sends a `KeepAlive` UPDATE
(`action=UPDATE, is_online=true`) every ~200ms, starting right after
"Session handshake complete" is logged. Per qc-mcp's
`transport.py::_start_heartbeat`, the QC stops streaming state (though
writes still work) once this lapses, so it runs on its own dedicated
`k_timer` + thread rather than piggybacking on the handshake thread: the
timer's expiry callback only signals a semaphore (it can't block, running in
interrupt-adjacent context), and a separate heartbeat thread does the actual
blocking `SET_REPORT` send. That send path (`QcSession::sendMessage()`) is
now shared between the handshake and heartbeat threads, guarded by a
`k_mutex` ([`ScopedKMutex`](app/src/ScopedKMutex.hpp)) so their reports can
never interleave on the wire. `KeepAliveMessage`'s `request_id`/`timeout`
fields are deliberately left unset in `QcHandshakeMessages.hpp::encodeKeepAlive()`
— qc-mcp's own reference implementation never sets them either, so they
never appear on the wire.

**Hardware verification outstanding, and the first attempt's log was inconclusive -
not confirmed.** A real Quad Cortex Mini run of this PR's build produced the exact
same handshake-then-silence trace #7 recorded *without* any heartbeat at all (41/331/
10/14-byte burst, then nothing), and a follow-up preset change on the unit produced no
console message either. **Correction to this section's first draft**: the claim below
that the device should keep "pushing unprompted state reads well past the post-connect
burst" was not actually grounded in anything - re-reading qc-mcp's `PROTOCOL.md` section
3 directly, step 4's burst is a one-time thing, and the heartbeat's documented job is
narrower: *"Writes land regardless of the heartbeat; reads require it."* Our firmware
doesn't send any reads of its own yet (that's #9/#5 Tier 2), so there's no "nothing
to show" expectation to begin with on its own - **except** that `PROTOCOL.md` is explicit
that a preset *recall* on the device itself autonomously pushes `RecallPreset` + `Scene`
+ `SetlistPosition` + `RecentsFavorites` + `PresetDirty:false`, gated on the session
being live the same way the rest of section 3 is. Getting *zero* console output after a
preset change is real evidence against "the heartbeat is working and the session is
live" - it just doesn't yet distinguish between these:

1. **The heartbeat isn't actually running/ticking** (a hung thread, a timer that never
   fires again after the first expiry, etc.) - previously had zero console visibility
   either way, since a successful or `-EPIPE`'d send (the documented quirk) logs nothing
   by design.
2. **The heartbeat runs but doesn't land** - wrong bytes, wrong command id, or the
   control-transfer send not actually reaching the device despite returning success.
3. **The device is ignoring/rejecting it** for a reason other than the bytes being wrong.
4. **Our own receive path stalled** - `qc_usbh_send_report()`'s control transfer and the
   continuous IN-endpoint receive loop share the USB host stack's single processing
   thread (see `usbh_shim.h`'s doc comment on `qc_usbh_send_report()`); a send every
   ~200ms contending with that shared thread, now happening continuously instead of 4
   times total during the one-shot handshake, is a real mechanism worth ruling in or out,
   not assumed.
5. The already-noted `gotReply=0` (Version reply never recognized during the handshake,
   falling back to `4.1.0`) might itself be a **real #7 bug**, not the already-accepted
   quirk it was treated as: a 41-byte message arrives in exactly that window, and nothing
   confirms whether that message *is* the Version reply arriving just too late for the
   500ms wait, or something unrelated.

This build adds diagnostics for all five, since the previous one had no way to tell them
apart from the console:

- `startHeartbeat()` now logs `KeepAlive heartbeat started (~200ms)` once, the first time
  the heartbeat thread is created - if this line is missing, the heartbeat thread was
  never started at all (rules out everything except "`runHandshake()` never reached the
  end" - check for a `Handshake: failed to send ...` error line right before it).
- `runHeartbeat()` now logs `KeepAlive heartbeat alive: N sent, last sendReport ret=R, M
  IN transfers` every 25 sends (~5s) - proves the heartbeat thread is still ticking (not
  hung) and shows what the control transfer is actually returning (`0`, `-EPIPE` per the
  documented quirk, or something else). `M` is `UsbHostClass::inTransferCount()`
  (new, `usbh_shim.c`) - the running count of IN-endpoint transfers completed, success or
  error, independent of whether anything was reassembled into a logical message. If `M`
  keeps climbing while nothing is ever reassembled, the receive path is alive but the
  device genuinely isn't sending meaningful data; if `M` goes flat with no
  `Failed to re-arm IN transfer` error either, the receive loop silently stalled - point
  4 above, not point 1-3.
- `handleMessage()` now logs `Message while awaiting Version reply: command=C, N bytes`
  for anything that arrives during the handshake's Version-reply wait that isn't
  recognized as one - settles point 5 directly: if `C` is `10` (`kCommandVersion`), the
  41-byte message *is* the real reply landing too late, a genuine #7 timing bug to fix
  separately; if it's something else, `gotReply=0` was correct and qc-mcp's own 150ms
  wait (vs. our 500ms) suggests this device answers fast, so a late reply would be
  surprising.

**Next hardware step**: flash this build, reproduce the same preset-change test, and
capture the **raw, unfiltered** console output (not just the `main`-module summary lines
used above) from "Session handshake complete" onward for at least 30-60s, including
whatever `usbh_shim`-module lines appear or stop appearing. That one capture should
settle which of the five explanations above is live, per the specific signatures named
next to each diagnostic line.

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

## Not yet done

- ETL is wired in via CMake `FetchContent` (see `app/CMakeLists.txt`) but not yet used by
  the main app - only by the HID framing tests above.
- The KeepAlive heartbeat (issue #8, see above) is implemented and unit-tested but **not
  yet confirmed on real hardware** - see that section's verification steps.
