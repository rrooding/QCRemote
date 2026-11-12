# Firmware (NXP FRDM-RW612, ESP32-S3)

Target boards: `frdm_rw612` (see [ADR 0002](../docs/adr/0002-devkit-selection.md) for why)
and `esp32s3_devkitc/esp32s3/procpu`, a Full Speed USB host and the likely RevA MCU (see
[ADR 0009](../docs/adr/0009-esp32-s3-second-target.md) and
["ESP32-S3 target"](#esp32-s3-target-issue-76) below). Same source tree; the board picks the
target. Zephyr, C++23 (see [CODING_STANDARDS.md](../CODING_STANDARDS.md)).

## Layout

```
firmware/
├── patches/zephyr/ # local fixes to apply to the Zephyr workspace (see "ESP32-S3 target")
└── app/            # the actual Zephyr application (out-of-tree, "freestanding")
    ├── CMakeLists.txt
    ├── Kconfig                     # app options: Wi-Fi, message buffer size, RW612 Full Speed test mode
    ├── prj.conf
    ├── boards/frdm_rw612.overlay   # switches the RW612's USB peripheral to host mode
    ├── boards/frdm_rw612.conf      # RW612 Wi-Fi driver + SoftAP settings
    ├── boards/esp32s3_devkitc_esp32s3_procpu{,_nopsram}.{overlay,conf}  # ESP32-S3, with/without PSRAM
    ├── src/
    │   ├── Main.cpp
    │   ├── usbh_shim.c/.h          # plain-C boundary around Zephyr's non-C++-safe USB host API
    │   ├── UsbHostClass.hpp        # generic C++ base built on the shim
    │   ├── QcHidBridge.hpp         # claims the QC Mini's HID interface, on top of UsbHostClass
    │   ├── HidReassembler.hpp      # qc-mcp framing: reports in, one logical message out
    │   ├── HidChunker.hpp          # qc-mcp framing: a message in, reports out
    │   ├── QcGzip.hpp              # issue #6: gzip detection + inflate (ADR 0005)
    │   ├── QcGzipInflater.hpp      # issue #6: runs QcGzip.hpp off the usbh thread
    │   └── QcMessageQueue.hpp      # issue #94: bounded SPSC hand-off from the usbh thread
    └── tests/
        ├── hid_framing/            # Ztest suite for HidReassembler/HidChunker - no hardware needed
        ├── gzip_inflate/           # Ztest suite for QcGzip.hpp, with real gzip fixtures
        ├── dispatch_queue/         # Ztest suite for QcMessageQueue/QcDispatcher, including a
        │                           # real preset-change burst on real threads
        └── wifi_web_http/          # Ztest suite driving the real HTTP/WebSocket resources over
                                    # the loopback interface - no Wi-Fi driver or board needed
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

### Or: use the pinned CI image (issue #46)

CI (`.github/workflows/firmware.yml`) doesn't do any of the above per run — each of its
jobs (`build-frdm-rw612`, `build-esp32s3`, `test-native-sim`, run in parallel) runs inside
`ghcr.io/rrooding/qcremote-firmware`, a container image (`ci/firmware/Dockerfile`) with the
pinned v4.5.0 workspace, both toolchains, the Wi-Fi/BLE blobs, and the ESP32-S3 DWC2 patch
(below) already baked in — 1.4 GB uncompressed (down from 2.75 GB after trimming the
workspace's git history and pip's download cache, neither of which anything in the image
reads again), pushed with zstd-compressed layers for a faster pull. You can use the exact
same image locally instead of the one-time setup above — pull the tag `firmware.yml`'s
jobs currently pin:

```bash
docker pull ghcr.io/rrooding/qcremote-firmware:v4.5.0-1
docker run --rm -v "$(pwd):/workspace" \
  ghcr.io/rrooding/qcremote-firmware:v4.5.0-1 \
  west build -b frdm_rw612 /workspace/firmware/app --pristine
```

The image's `WORKDIR` is already the Zephyr workspace, so `west build` just needs this repo
bind-mounted in. If `docker pull` fails with an authentication error, either `docker login
ghcr.io` with a personal access token that has `read:packages`, or ask whoever owns the
`rrooding` GHCR namespace to make the `qcremote-firmware` package public (Package settings →
Change visibility) — CI itself doesn't need this, since `GITHUB_TOKEN` already has read access
to a package owned by the same account.

**Bumping the pinned Zephyr version or toolchain** is the one deliberate place this drifts:
edit `ZEPHYR_VERSION`/`--gnu-toolchains` in `ci/firmware/Dockerfile`, dispatch "publish
firmware CI image" (Actions tab → that workflow → Run workflow) with a new `version` (e.g.
`v4.6.0-1`), then update the image tag in both `firmware.yml` and the command above to match.

## Build

```bash
west build -b frdm_rw612 /path/to/QCRemote/firmware/app --pristine
```

Drop `--pristine` on subsequent builds once the build directory exists. For the ESP32-S3,
apply the Zephyr patch first and use its own build directory, so the two targets don't
overwrite each other - see ["ESP32-S3 target"](#esp32-s3-target-issue-76).

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

- `QcDispatcher` and `QcGzipInflater` each had a single in-flight mailbox slot (see their own
  header comments) - a message arriving while the previous one on that path was still being
  processed, or one too big for the mailbox, was dropped. Both count every drop
  (`droppedCount()`, a lifetime total); `QcSession` snapshots that count when the File stream
  starts and diffs it at the end, so the summary's `dispatcherDropped`/`gzipDropped` report only
  what was lost *during this stream*. **Measured on real hardware (commit `0d1edcc`): both 0**
  across a 551-message, ~12s stream - one slot kept up with the File stream's own arrival rate.
  It did **not** keep up with a preset-change burst: see
  ["Dispatch queue (issue #94)"](#dispatch-queue-issue-94) below, where `QcDispatcher`'s slot
  became a bounded queue. `QcGzipInflater` still has one slot, deliberately.
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

- **`DD`/`DG` (dispatcherDropped/gzipDropped) above 0**: `QcDispatcher`'s queue or
  `QcGzipInflater`'s single slot couldn't keep up with the device's arrival rate during this
  stream. 0 on the hardware run above; `N` is then a lower bound on what the device actually sent
  if this is ever nonzero on a future run. Both counts on this line are scoped to the stream;
  `checkTrafficHealth()`'s own warning reports lifetime totals plus a per-warning delta - see
  ["Dispatch queue (issue #94)"](#dispatch-queue-issue-94).
- **`RO`/`RM` (reassemblyOverflowCount/reassemblyMalformedCount) above 0**: confirmed nonzero on
  real hardware (31/2850) - see "Hardware results" above.
- **`MW`/`MI` (maxWireBytes/maxInflatedBytes)**: the largest single message actually seen,
  successfully processed. If `DD`/`DG`/`RO` are all 0 this is the true peak; otherwise the true
  peak is *at least* this large, since a dropped message never reaches this tracking.
- **A crash or stack fault**: capture the fault PC/LR and `addr2line` against `zephyr.elf`, same
  method used to find issue #8's `usbh`-stack overflow (see that section above) - and say which
  thread's name appears in the dump (`qc_dispatch`, `qc_gzip_inflate`, `qc_session`,
  `qc_heartbeat`, or `usbh` itself).

## File reassembly/inflate buffer sizing (issue #33)

Issue #33 asked for real sizes for `kReassemblyCapacity` (`QcHidBridge.hpp`), the dispatcher
mailbox (`QcDispatcher.hpp`), and `kMaxCompressedInputSize`/`kMaxInflatedSize` (`QcGzip.hpp`) -
all originally 4096/4096/4096/16384, placeholders with no real traffic measured against them.
Getting there took several hardware rounds (measuring the original placeholders' overflow by
command, then testing whether `FileMessage` READ's `short_listing`/`omit_factory_content`
options could shrink the listing before sizing anything) - that measurement history, the
hardware evidence it produced, and the final sizing decision are all in
[ADR 0007](../docs/adr/0007-file-reassembly-buffer-sizing.md). This section covers only the
**final, shipped behavior** and what's confirmed on hardware for it.

**Decision**: `FileMessage` READ always sends `omit_factory_content=true` (confirmed on
hardware to shrink the owner's listing from ~550 messages/~716KB to 20 messages/~30KB while
still naming every user/cloud preset - `short_listing` measured no effect and is no longer
sent). `kReassemblyCapacity`/the dispatcher mailbox are 131072 bytes (128 KiB, sized for a
worst-case fully-named 256-preset folder, not just the owner's own mostly-empty one).
`kMaxInflatedSize` is 32768 bytes (32 KiB); `kMaxCompressedInputSize` is 8192 bytes. The
measurement-only variant experiment, its per-folder tables, `CONFIG_THREAD_ANALYZER`, and the
per-command overflow/size tallies that existed only to gather sizing evidence are all removed.
`ModelRepo`/`ModuleStats` replies are now ignored outright (counted, not buffered) rather than
dispatched - the preset listing doesn't need either, and both are large enough (59219/28354
bytes, measured) that buffering them cost real RAM and gzip-inflater contention for no benefit.
See ADR 0007 for the full reasoning, including the RAM cost this carries (measured: 371272 /
983040 bytes, 37.77%, up from the pre-#33 baseline's 9.95% - a real, deliberate tradeoff worth
weighing against issue #34's note that a smaller board is a possible later move) and the
stack-margin re-check for every thread that touches these buffers.

**What's confirmed on hardware, and what isn't.** Every hardware number ADR 0007 cites (the
~550-message default listing, the 20-message `omit_factory_content` listing, the 10193-byte
largest message, the 22667-byte `RecallPreset`, the `ModelRepo`/`ModuleStats` overflow sizes,
the three stack overflows this issue found and fixed along the way) came from real runs against
*earlier* commits of this PR - each one a smaller, targeted change, individually verified on
hardware before the next step. **This PR's final tip - the cleaned-up build with the sizes
above, the ModelRepo/ModuleStats ignore change, the always-5000ms quiet window, and the
simplified `HidReassembler`/`QcSession` this ADR describes - has not itself been run on real
hardware.** It differs from every commit that was actually flashed: smaller (the whole
measurement scaffold is gone) but also different in a few real ways (no more per-variant
sending, `ModelRepo`/`ModuleStats` no longer reach the dispatcher at all, the reassembly/gzip
buffers are bigger than any previously-tested build used). Treat it as "built from
hardware-verified pieces," not "hardware-verified as a whole," until the smoke test below runs
on a real unit.

### Smoke test for the next hardware run

1. Flash this build. Handshake should complete and mirror firmware version `4.1.1`, same as
   every prior run on this unit.
2. The `File` stream should finish **quiet** (not timeout) with **about 20 messages**, no
   `File stream limits:` line showing a nonzero `dropped`/`reassembly overflow`/`malformed`
   count - a nonzero count there means the listing is incomplete. `My Presets` (21 presets),
   `Practice` (2 presets), and `cloud-0-1` should all be present in that count (not printed by
   this firmware - printing the decoded list is issue #10's job, not this one's).
3. Changing the active preset, **including to "Practice 1"** (the ~22667-byte `RecallPreset`
   this issue sized `kMaxInflatedSize` against), should inflate successfully - no
   `Inflate failed ... OutputTooLarge` line for it.
4. No crash, and no `Traffic drop/overflow:` log line, over a couple of minutes of normal use
   (idle plus at least one preset change) after the stream finishes.

Any deviation from the above - a `File stream limits:` line with a nonzero count, an
`OutputTooLarge` on a real preset recall, or a crash - is worth a comment on #33 with the full
log before assuming the sizing is wrong; it may instead be a bug in this cleanup pass itself
(this build hasn't run on hardware at all yet, per the note above).

## ESP32-S3 target (issue #76)

The same app builds for the ESP32-S3-DevKitC-1 (`esp32s3_devkitc/esp32s3/procpu`). The S3 is a
**Full Speed** (12 Mbit/s) USB host; the RW612 is High Speed. Verified on hardware against a
Quad Cortex Mini on CorOS 4.1.1: the handshake mirrors `4.1.1`, and the File stream is
identical to the RW612's (21 messages, 29599 wire bytes, largest 10193, 0 malformed, 3
folders / 48 presets, ~5.7 s). Why this is a target at all: [ADR 0009](../docs/adr/0009-esp32-s3-second-target.md).

### One-time: patch the Zephyr workspace

Zephyr v4.5.0's DWC2 host driver (the S3's USB controller) only starts a waiting interrupt
transfer when the next frame number *exactly* matches the one it scheduled. Its SOF events
are coalesced, so a late driver thread steps past that frame and the endpoint stalls for a
whole frame-counter wrap: **~16.4 s with no IN data** after every connect, long enough for
the Quad Cortex to drop the Version reply and much of the File stream. The patch starts any
channel whose frame is now or already past:

```bash
git -C ~/zephyrproject/zephyr apply /path/to/QCRemote/firmware/patches/zephyr/0001-uhc_dwc2-start-late-periodic-channels.patch
```

It only touches `drivers/usb/uhc/uhc_dwc2.c`, so RW612 builds aren't affected. Undo with
`git -C ~/zephyrproject/zephyr checkout drivers/usb/uhc/uhc_dwc2.c`. Not yet upstream; the
build succeeds without it, but the S3 then shows the 16 s stall above.

### Build, flash, monitor

The S3 board config depends on whether the module has PSRAM. Check with
`python3 -m esptool --port /dev/cu.usbserial-XXXX chip-id`: an R2/R8 module lists
`Embedded PSRAM` under Features; an N8/N16 module doesn't.

| Module | Build | What runs |
|---|---|---|
| **With 8 MB octal PSRAM** (WROOM-1-N8R8/N16R8; RevA's N16R8) | default | USB bridge **and** Wi-Fi/web transport (issue #71). The app's static data (`libapp.a` `.bss`, incl. the 128 KiB message buffers) lives in PSRAM. |
| **Without PSRAM** (WROOM-1-N8/N16) | `-DFILE_SUFFIX=nopsram` | USB bridge only. Wi-Fi doesn't fit internal RAM (~160 KB over), so it's off; message buffers are 64 KiB. |

**The default (Wi-Fi) build is CI-build-only on the S3 today** - verified to build and link
clean (`CONFIG_BUILD_ONLY_NO_BLOBS`, no Espressif Wi-Fi blobs fetched), but none of this
firmware's Wi-Fi/web-transport hardware results (see "Wi-Fi host/client mode" below) have been
run against the S3 - every one of them is from the `frdm_rw612`. The S3's own hardware
verification to date (issue #76/PR #77) is USB-only, with Wi-Fi off (the `nopsram` config).

The default (Wi-Fi) build also needs Espressif's Wi-Fi/BT blobs. Zephyr checks for the whole
set, not just the S3's (~40 MB, no license click-through):

```bash
west blobs fetch hal_espressif
```

```bash
west build -b esp32s3_devkitc/esp32s3/procpu /path/to/QCRemote/firmware/app -d build-s3 --pristine
west build -b esp32s3_devkitc/esp32s3/procpu /path/to/QCRemote/firmware/app -d build-s3 --pristine -- -DFILE_SUFFIX=nopsram   # no PSRAM
west flash -d build-s3
west espressif monitor
```

Wi-Fi mode and credentials come from `wifi_credentials.conf`, the same as on the RW612 (see
"Wi-Fi host/client mode" below). In AP mode the ESP32 driver, unlike NXP's, doesn't run a
DHCPv4 server, so the app sets the AP's address and starts one itself
(`CONFIG_QC_WIFI_AP_APP_DHCP_SERVER`, on automatically for non-NXP drivers; address
`CONFIG_QC_WIFI_AP_IP_ADDRESS`, default 192.168.4.1, the same range as the RW612). The NXP
driver's own Wi-Fi settings moved from `prj.conf` to `boards/frdm_rw612.conf`.

Flash and log over the board's **UART** micro-USB port (CP2102N bridge, `zephyr,console` is
UART0). If `west flash` doesn't find it, add `--esp-device /dev/tty.usbserial-XXXX`. The ROM
loader's `SHA-256 comparison failed ... Attempting to boot anyway` line on boot is expected
with Zephyr's simple-boot image.

### Wiring the Quad Cortex

The board's native "USB" micro-USB port only takes power in - it has no VBUS output - so the
Quad Cortex is wired from the header pins instead, with VBUS from the 5V pin. Leave the native
port unplugged; it's in parallel with GPIO19/20.

| Signal | USB colour | USB-A pin | DevKitC-1 header |
|---|---|---|---|
| VBUS | red | 1 | J1 pin 21 (5V) |
| D- | white | 2 | J3 pin 20 (GPIO19) |
| D+ | green | 3 | J3 pin 19 (GPIO20) |
| GND | black | 4 | J3 pin 21 (G) |

Check the colours with a meter before connecting - VBUS on a GPIO damages the S3. From the
USB-A plug, a USB-C-plug-to-USB-A-socket adapter into the Quad Cortex worked on the owner's
unit. The Quad Cortex draws no current from VBUS; it only needs it present.

### What the S3 needed (all in this tree, all harmless on the RW612)

- **Joining split reports** (`QcHidBridge::onReportIn()`): the DWC2 driver completes an
  interrupt IN transfer per packet, so with `wMaxPacketSize 128` each 129-byte report arrives
  as 128 + 1 bytes. The RW612's EHCI driver delivered whole reports, which the reassembler
  requires.
- **Connect pacing** (`QcSession::waitForInTrafficToSettle()`): the File READ now waits until
  the 12 state READs' reply burst (ModelRepo ~59 KB, ModuleStats ~28 KB) has drained. At Full
  Speed the burst runs close to the endpoint's ceiling (two packets per report, one packet per
  1 ms frame), and the device drops reports it can't send rather than waiting. With the File
  READ sent into the burst, messages merged and the catalog came back empty. Logged as
  `State READ replies settled after N ms (M IN transfers)`; at High Speed it costs ~1-2 s.
- **Send/disconnect race** (`qc_usbh_send_report()`): a send blocked in `usbh_req_setup()`
  when the device disconnected then freed its buffer through the already-NULLed
  `claimed_udev`. It now holds a local copy. Not S3-specific; it just showed up there first.
- **RAM**: the two 128 KiB message buffers (ADR 0007) don't fit the S3's internal RAM.
  `CONFIG_QC_MESSAGE_CAPACITY` (app Kconfig, default 131072) sets both. The PSRAM board
  config keeps 131072 in PSRAM; the `nopsram` one uses 65536, which still holds every message
  measured on the owner's unit (largest 22667).
- **Heartbeat LED**: the board's own LED is addressable RGB, so the overlay puts `led0` on
  GPIO2 (J3 pin 5) - optional.

`sendMessage()` also now logs any `SET_REPORT` that takes over 50 ms, since a slow send holds
the send lock and delays the KeepAlive behind it.

### Testing Full Speed on the RW612

`CONFIG_QC_USBH_FORCE_FULL_SPEED=y` (RW61x only, off by default) sets `PORTSC1.PFSC`, so the
RW612 never negotiates High Speed and stands in for a Full Speed host. Set the bit before the
device attaches: boot with the Quad Cortex unplugged and plug it in after
`USB host forced to Full Speed` is logged. A board reset with it already plugged in comes up at
High Speed. The probe log then shows `Port speed (PSPD): 0` (Full; 2 = High).

```bash
west build -b frdm_rw612 /path/to/QCRemote/firmware/app -d build-fs --pristine -- -DCONFIG_QC_USBH_FORCE_FULL_SPEED=y
```

## Wi-Fi host/client mode and a web/WebSocket transport (issue #71)

Issue #71 tests whether a web UI served from the board over Wi-Fi is viable enough that the
owner would go with web UI + Wi-Fi instead of the planned JUCE + BLE macOS/iOS app (MVP2). See
[ADR 0010](../docs/adr/0010-wifi-web-transport.md) for the full design and the feasibility
findings (Zephyr 4.5 supports both Wi-Fi AP and station mode on `frdm_rw612` - confirmed
against the Zephyr source tree and its own `samples/net/wifi/apsta_mode` sample, not assumed).
This section covers setup, build/flash, and the exact hardware test for each mode.

**Go: the owner has decided Wi-Fi is a viable transport, and MVP2 moves to a web UI over
Wi-Fi.** That decision is based on this build's (`b735057`) station-mode hardware result: from
a Mac, Quad Cortex Mini connected, the board joined the home network, the page loaded with the
full 3-folder/48-preset catalog, and USB/handshake/heartbeat/`File` listing were unaffected (21
messages, no drops) over about 3 minutes with no fault. See ADR 0010's "Hardware findings
(`b735057`), and the go decision" section for the full writeup, including what this result
does and doesn't cover - in particular: **access point mode last came up on an earlier build
(`26bfec1`, a phone joined) and hasn't been retested since**, **no iPhone has been tested in
either mode**, and **live WebSocket updates don't work** - a client gets the catalog on page
load, but a preset change on the unit doesn't push a live update; that's split out to
[issue #75](https://github.com/rrooding/QCRemote/issues/75) rather than blocking this one. The
steps below are still the exact test to repeat for anything not yet covered by the result
above.

### Setup: Wi-Fi mode and credentials

Copy the example file and edit it - it's gitignored (`firmware/.gitignore`) and picked up
automatically by `app/CMakeLists.txt` as an extra Kconfig fragment, no extra `west` flag
needed:

```bash
cp firmware/app/wifi_credentials.conf.example firmware/app/wifi_credentials.conf
```

Pick **one** mode and fill in real values (see the file's own comments):

```kconfig
# Access point - the board creates its own Wi-Fi network:
CONFIG_WIFI_USAGE_MODE_AP=y
CONFIG_QC_WIFI_AP_SSID="QCRemote"
CONFIG_QC_WIFI_AP_PSK="pick-a-real-wpa2-passphrase"

# Station - the board joins your existing Wi-Fi network:
CONFIG_WIFI_USAGE_MODE_STA=y
CONFIG_QC_WIFI_STA_SSID="YourNetworkName"
CONFIG_QC_WIFI_STA_PSK="YourNetworkPassword"
```

**Never commit `wifi_credentials.conf`** - only the `.example` file is tracked. Leaving
`CONFIG_QC_WIFI_AP_PSK` empty starts an *open* access point; this build defaults to a real
WPA2 passphrase when one is set rather than defaulting to open (this firmware's own choice, not
something issue #71 asked for specifically - the firmware logs a warning if it starts open
anyway).

The RW612's own Wi-Fi/BLE radio firmware is a licensed binary blob, fetched the same way ADR
0002 already documents for BLE - if not already done:

```bash
west blobs fetch hal_nxp
```

### Build

Same as the plain build above, once `wifi_credentials.conf` is in place:

```bash
west build -b frdm_rw612 firmware/app --pristine
```

Both modes build clean; see ADR 0010 for the measured RAM/flash cost (a real, large jump - the
single biggest of any issue so far in this project, from bringing in Zephyr's networking
stack, Wi-Fi driver, HTTP server, and mbedTLS).

### Hardware test - access point mode

1. Set `CONFIG_WIFI_USAGE_MODE_AP=y` in `wifi_credentials.conf` with a real SSID/passphrase,
   build, and flash.
2. Watch the serial console: `Wi-Fi AP "<SSID>" starting (DHCPv4 server is driver-managed...)`,
   a `Startup summary: mode=AP ssid="<SSID>" http_server=listening port=80` line, then
   `Wi-Fi AP enabled - waiting for a station to join` followed by a second
   `Startup summary: mode=AP iface=up ip=192.168.4.1 dhcp=driver-managed ...
   url=http://192.168.4.1/` line, and (once something joins) a
   `Wi-Fi AP: station <MAC> joined` line. On `ac601b2` neither the enable-result nor the "AP
   enabled" line ever appeared, and `nxp_wifi: DHCP Server start failed` did - see ADR 0010's
   "Hardware findings" for the confirmed cause (an app-layer DHCPv4 server conflicting with the
   driver's own) and the fix applied here. **Confirmed fixed on `26bfec1`**: "AP enabled" and
   the second startup-summary line both appeared.
3. On an iPhone and separately on a Mac: open Wi-Fi settings and look for the board's SSID in
   the list before anything else - on `ac601b2` it never appeared at all. **Confirmed on
   `26bfec1`**: "QCRemote" appeared and a phone joined. (Which device joined wasn't recorded at
   the time; an iPhone specifically has not been confirmed in this mode - see step 4.)
4. Open `http://192.168.4.1/` in a browser on each device. Expect the folder/preset listing
   from the catalog (same content the serial console already prints, per issue #10) and a
   `presets: N (generation G)` status line. On `26bfec1` this step crashed the board outright -
   a USAGE FAULT / stack overflow on thread `rx_q[0]` - in both AP and station mode, with or
   without a Quad Cortex attached. Root cause confirmed from source and fixed (this build raises
   `CONFIG_NET_RX_STACK_SIZE`; see ADR 0010's "Hardware findings (`26bfec1`)"), and **confirmed
   fixed on `b735057` in station mode** (see the go-decision result below) - but **AP mode's own
   page load hasn't been retried against this fix**, since the owner's `b735057` run was station
   mode only. This step, specifically, is the first thing to check on a fresh AP-mode run. If it
   crashes with the same `rx_q[0]`/stack-overflow signature, paste the full fault register dump
   (especially PC/LR) on the issue rather than re-guessing - it would mean the measured ~1136 B
   call-chain estimate in that ADR section was still short.
5. With the page open, change the active preset or folder on the Quad Cortex Mini itself.
   **Known broken** ([issue #75](https://github.com/rrooding/QCRemote/issues/75)): the page
   does not update live. The catalog it loaded on step 4 stays as-is until manually reloaded -
   this is not the coexistence/stability check, just a known gap out of scope for this issue's
   go/no-go decision.
6. Leave it running for at least 10-15 minutes with the Quad Cortex Mini connected over USB
   (handshake complete) and the web page open: watch for KeepAlive/File-stream behavior
   unaffected by the AP running (`qc_heartbeat`'s usual ~200ms cadence, no new
   `File stream limits:`/`Traffic drop/overflow:` lines), and for the Wi-Fi link itself
   staying up (no repeated `Wi-Fi AP: station ... left` and `joined` for the same device,
   which would mean it's dropping and reconnecting).

**What each outcome means**: no "AP enabled"/second startup-summary line at all repeats
`ac601b2`'s DHCP conflict - paste the console output and comment on the issue rather than
guessing further. A page that loads once but never pushes updates points at the WebSocket path
(`QcWifiWebTransport::broadcast()`); a client stuck with no IP despite "AP enabled" having
logged points at the driver's own DHCP server rather than this firmware; any
`qc_heartbeat`/`qc_dispatch`/USB-side regression while Wi-Fi is active (vs. a build with Wi-Fi
disabled) would be the coexistence requirement failing, not just a Wi-Fi bug.

### Hardware test - station mode

1. Set `CONFIG_WIFI_USAGE_MODE_STA=y` with the real network's SSID/passphrase, build, and
   flash.
2. Watch the serial console: `Joining Wi-Fi network "<SSID>"`, then either
   `Wi-Fi station connected: Connection successful` followed by a
   `Startup summary: mode=STA iface=up ip=<ip> dhcp=Zephyr DHCPv4 client ... url=http://<ip>/`
   line (Zephyr's own DHCPv4 client assigns that IP automatically -
   `CONFIG_WIFI_STA_AUTO_DHCPV4`), or a `Wi-Fi station connect failed: <reason>` warning. On
   `ac601b2`, this firmware logged "connected" even around a genuine
   `nxp_wifi: WLAN: nxp_wlan_network authentication failed` warning - that was this firmware
   not reading the event's own status (see ADR 0010's "Hardware findings"), now fixed; a
   "connected" line should mean it actually connected.
3. Find the board's IP address - easiest from the router's own client list (DHCP leases),
   since nothing here advertises it, or from the `ip=` field in the startup-summary log line
   above. (A follow-up issue could add mDNS/`.local` advertising if this test succeeds; out of
   scope for this one.)
4. From an iPhone and a Mac already on the **same** network, open `http://<that IP>/` in a
   browser. **Verified on `b735057` from a Mac**: the page loads with the full catalog (3
   folders, 48 presets). **Not tested**: an iPhone, in this mode or AP mode. **Known broken**
   ([issue #75](https://github.com/rrooding/QCRemote/issues/75)): a preset change on the unit
   does not push a live update over the WebSocket - the page stays as it loaded until manually
   reloaded.
5. Same 10-15 minute soak as AP mode: confirm KeepAlive/File-stream/USB behavior is unaffected,
   and the station stays connected. A
   `Wi-Fi station link dropped and is re-associating` warning here is the NXP stack reporting an
   AP-initiated disassociation, not a credential problem - see "What each outcome means" below.

**What each outcome means**: same as AP mode above, plus - `Wrong password`
(`WIFI_STATUS_CONN_WRONG_PASSWORD`) is the *only* status the NXP stack has for a station
losing its association, and it reports an AP-initiated disassociation/deauthentication with it
just as readily as an actually-wrong passphrase: the middleware's
`wlcm_process_disassoc_event()` calls `do_connect_failed(WLAN_REASON_NETWORK_AUTH_FAILED)`
without ever looking at the 802.11 reason code (confirmed from source - see ADR 0010's
`5068403` findings). Mid-session it is almost never a credential problem, and the stack
re-associates on its own; this firmware now says so in the log rather than repeating the
vendor wording. If the *underlying* 802.11 reason is ever needed, build with
`CONFIG_WIFI_LOG_LEVEL_DBG=y`, which turns on the middleware's own
`got event: disassociation, code=<N>` line. The owner has confirmed "Radioshack" is WPA2
(not WPA3/mixed) on both bands under the same SSID, which rules out a security-type mismatch -
this build's request was already using the matching `WIFI_SECURITY_TYPE_PSK`. A station that
connects but the board never gets a reachable IP from the router's own DHCP pool is a
network-side problem to check from the router, not this firmware.

### Hardware results and the go decision

**Verified on `b735057`**, station mode, from a Mac, Quad Cortex Mini connected: joined the
home network and got an IP from DHCP; HTTP server listening; page loaded with the full
catalog (3 folders, 48 presets); USB, handshake (`4.1.1`), heartbeat, and `File` listing
unaffected (21 messages, no drops); about 3 minutes with no fault; a preset change arrived over
USB as before. This is the result the owner's go decision - Wi-Fi is a viable transport, MVP2
moves to a web UI over Wi-Fi - is based on. Full writeup: ADR 0010's "Hardware findings
(`b735057`), and the go decision".

**Not covered by that result, and still exactly as open as the sections above left it**:

- Access point mode on this build - last came up on `26bfec1` (before the `rx_q[0]` fix this
  build carries), not retried since.
- An iPhone, in either mode - every result above is from a Mac.
- The full 10-15 minute soak this section's own steps ask for - the verified run was ~3
  minutes.
- Live WebSocket updates - known broken, split out to
  [issue #75](https://github.com/rrooding/QCRemote/issues/75), not part of this decision.

**The cleaned-up build differs from `b735057`**: the commit this hardware result is pinned to
(`b735057`) still carried `CONFIG_THREAD_ANALYZER` and its periodic stack-dump logging, added
for this issue's own measure-and-report requirement. Those are removed in the current commit
now that the measurements they produced (ADR 0010's stack-margin tables) are recorded - the
underlying fix they helped verify (`CONFIG_NET_RX_STACK_SIZE=3072`) is unchanged. The smoke
test below is what to re-run against the current commit before trusting it the same way.

### Smoke test of `5068403`, and what it found

The owner's smoke test of `5068403` found USB/handshake/`File` fine (21 messages, no drops, 3
folders / 49 presets) and two problems - the page never showing presets, and a "Wrong password"
warning 52 s into a working session on a correct passphrase. Both are now reproduced off-board
and fixed; ADR 0010's "Hardware findings (`5068403`)" has the full trace. In short:

- `GET /api/presets.json` answered with a status line of `HTTP/1.1 200` - no space, no reason
  phrase - because Zephyr builds a *dynamic* resource's status line from
  `"HTTP/1.1 %d%s%s\r\n"` and leaves both `%s` empty unless
  `CONFIG_HTTP_SERVER_COMPLETE_STATUS_PHRASES` is on. RFC 9112 §4 requires that space. The
  static page resource was never affected (it sends a hardcoded `HTTP/1.1 200 OK`), which is
  exactly why the page loaded and its own fetch didn't. Fixed in `prj.conf`, and pinned by a new
  Ztest suite - `firmware/app/tests/wifi_web_http` drives the real resources over a real socket
  on the loopback interface with real browser headers, and fails without the option.
- A newly connected WebSocket client used to receive nothing until the next publish (once per
  device connect, or one throttled push per minute) - it now gets the catalog immediately.
- The page itself had no error path and no retry. It now reports what failed in its own status
  line, retries the fetch, and reconnects the WebSocket. The firmware logs every
  `GET /api/presets.json` it answers (`Served GET /api/presets.json (<N> bytes)`) and every push
  (`Pushed catalog to websocket client <fd> (<N> bytes)`).

### Smoke test for this build

1. Flash the current commit in station mode (`CONFIG_WIFI_USAGE_MODE_STA=y` with a real
   SSID/passphrase), with the Quad Cortex Mini connected over USB.
2. Confirm the usual boot/USB/handshake traces (see "Verify" above), then
   `Joining Wi-Fi network "<SSID>"`, `Wi-Fi station connected: Connection successful`, and a
   `Startup summary: ... ip=<ip> ... url=http://<ip>/` line.
3. From a browser on the same network, open that URL. The page should show the real
   folder/preset listing (49 presets on the owner's unit) **within a second or two**, and the
   console should show the fetch being served and the WebSocket being pushed to, in either
   order:

   ```
   <inf> main: Served GET /api/presets.json (NNNN bytes)
   <inf> main: Websocket client connected (sock=N)
   <inf> main: Pushed catalog to websocket client N (NNNN bytes)
   ```

   If the page still says `could not load presets (...)`, that message names the failure
   (an HTTP status, or a JSON error) - paste it along with whether the `Served GET` line
   appeared, which separates "the browser never asked" from "the firmware answered and the
   browser rejected it".
4. Leave it running a few minutes with the web page open: confirm no `LOG_ERR` lines, no
   `qc_heartbeat`/`qc_dispatch`/USB-side regression, and the Wi-Fi link stays up. A
   `Wi-Fi station link dropped and is re-associating` warning is the vendor-stack misreport
   described above and is expected to be followed by `Wi-Fi station connected`; the page should
   recover on its own (`websocket closed - reconnecting...` then the listing again).
5. `Post-connect tally: command=33 count=<N>` stepping up once a minute is `GlobalTempo`
   streaming at ~3/s, as `docs/PROTOCOL-NOTES.md` records - expected, not a preset change.

### Known limitations of this test build

- **Live WebSocket updates, and the shared-receive-buffer hazard** - both fixed by
  [issue #75](https://github.com/rrooding/QCRemote/issues/75); see its own section below rather
  than restating it here.
- **No authentication** on the HTTP/WebSocket endpoints - matching issue #71's own stated
  scope ("home use, not gigging... security can come later").
- **Plain HTTP, not HTTPS** - also matching that stated scope; `CONFIG_HTTP_SERVER`'s TLS
  support isn't enabled.
- **AP mode is 2.4 GHz only** - the driver's auto-channel-select only reads the connect
  request's band hint when picking a 5 GHz channel, and 5 GHz AP operation brings DFS/
  regulatory channel-vacating requirements this firmware does nothing to handle; not something
  the owner asked for. Station mode has no band restriction - confirmed by reading
  `nxp_wifi_connect()` (the station connect path never reads the connect request's `band`
  field at all), so an earlier version of this code setting it to 2.4 GHz for station mode was
  always a no-op, not an actual restriction; issue #71's own text only says "2.4 GHz
  reliability... [is] acceptable," not that 5 GHz should be refused.
- **The page is deliberately minimal** - a folder/preset listing and a status line, nothing
  resembling the real Quad Cortex's own UI yet (issue #71's own scope: prove the transport,
  not design the product).
- **AP and STA are build-time exclusive here** - Zephyr's own `apsta_mode` sample shows running
  both at once is possible on this chip, but issue #71 only asks for build-time mode choice;
  simultaneous AP+STA is a possible future enhancement, not built now.

## Live WebSocket updates (issue #75)

Issue #71's own build already pushed the catalog to a connected browser on connect and on a
throttled schedule (see the "Known limitations" note above, now resolved). This issue:

1. **Root-caused why a *repeat* preset change still didn't reach the page within a couple of
   seconds.** `QcSession::runHeartbeat()`'s post-connect tally forwards a *new* command
   immediately, but gated every *repeat* bump behind one window shared across every tracked
   command (`kPostStreamLogIntervalMs`, 60 s). `GlobalTempo` ticks ~4/s and resets that shared
   window every time it's forwarded, so a real preset switch landing in between had to wait for
   `GlobalTempo`'s own next forward to reopen it - up to a minute later. That's exactly the
   `5068403` smoke test's `command=33 count=2` → `count=170` a minute apart, and the page's
   `lastChange` mislabeling a `GlobalTempo` tick as "preset change". Fixed:
   `QcMessageTally.hpp`'s new `shouldForwardTallyEntry()`/`isPresetChangeCommand()` let a
   preset-identity command (`RecallPreset`, `Scene`, `Grid`, `RecentsFavorites`, `PresetDirty`,
   `SetlistPosition` - this file's own pre-existing "preset-change pushes" list) forward on
   *every* bump, never throttled, while a continuously-streaming command (`GlobalTempo`,
   `MasterVolume`, ...) keeps the original once-per-minute behavior unchanged.
2. **Adopted ADR 0011's envelope** (`QcLiveUpdateEnvelope.hpp`):
   `{"v":1,"t":"<type>","seq":N,...}`, with the three outbound message types and the one inbound
   one this issue needs:
   - `snapshot` - the full catalog, sent to a newly-connected client and whenever the catalog
     itself changed (`publishCatalog()`).
   - `patch` - just `lastChange` (command/count), sent when a preset-change event arrived but the
     catalog didn't (`publishPresetChange()`) - a few dozen bytes instead of resending the whole
     ~2 KB listing.
   - `tick` - liveness only, sent every `CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS` (5000 by default,
     matching `web/poc/src/store.js`'s own assumption) when nothing else was sent in that window.
   - `resync` (inbound) - sent by the page after it notices a `seq` gap; answered with a fresh
     `snapshot`. The full message schema is #15; this firmware only emits/recognizes these four.
3. **Fixed the shared WebSocket receive buffer** (ADR 0010/0011's "left to #75"). Re-confirmed
   against the pinned v4.5.0 tree: Zephyr hands this resource's one static `data_buffer` to
   `websocket_register()` on every upgrade, so two concurrently open browsers really would share
   one receive buffer and could corrupt each other's partially received frames - and since this
   issue adds a client-to-server message (`resync`), "the page never sends" stopped being a way
   out of it. Fixed by capping it at one live client: `CONFIG_WEBSOCKET_MAX_CONTEXTS=1` (matching
   `QcWifiWebTransport::kMaxWsClients=1`) makes Zephyr itself refuse a second concurrent upgrade
   (`websocket_get()` finds no free context) before any of this firmware's own code runs - a
   second static resource/buffer pair was the alternative, but it would need the page to know
   which URL is free, for two-simultaneous-live-clients support nothing has asked for.

### Verified

`west twister -p qemu_cortex_m3 -p mps2/an385 -K -T firmware/app/tests`: all 11 suites pass on
`mps2/an385` (234/234 cases, including the 2 new ones below). `qemu_cortex_m3` fails only on the
new `live_updates` suite with a timeout - the same "too little RAM for a full
`QcPresetCatalog`/`QcCatalogSnapshot` pair" limitation `preset_transport`'s own testcase.yaml
already documents for that platform (both suites exclude it from their own `platform_allow` and
only run there at all because of `-K`/`--force-platform`). `native_sim` couldn't be run locally
(needs a Linux host - this was verified from a Mac); it's what CI actually gates on.

Two new Ztest suites: `tests/live_updates` (pure-logic coverage for the envelope encoding, the
`shouldForwardTallyEntry()`/`isPresetChangeCommand()` tally fix, and `resync` detection - no
Zephyr networking needed) and four new cases added to `tests/wifi_web_http` (the enveloped
snapshot push, a preset-change patch with no catalog resent, a tick after the configured
interval, and a resync producing a fresh snapshot - all driven over the real HTTP/WebSocket
resources on the loopback interface, same as that suite's existing cases).

Clean builds with example credentials in a separate build tree (this worktree's real
`wifi_credentials.conf` was never created, read or committed), compared against `595a0a3`
(current master) built the same way:

| Config | RAM (internal) before → after | Flash before → after |
| --- | --- | --- |
| `frdm_rw612`, STA | 734064 B / 74.67% → 734048 B / 74.67% (**-16 B**) | 719800 B → 722304 B (**+2504 B**) |
| `frdm_rw612`, AP | 741368 B / 75.42% → 741352 B / 75.41% (**-16 B**) | 746444 B → 748948 B (**+2504 B**) |
| ESP32-S3, with PSRAM | `dram0` 200624 B → 200536 B (**-88 B**) | `irom0`+`drom0` 543956 B → 546580 B (**+2624 B**) |
| ESP32-S3, no PSRAM | `dram0` 333424 B → 333424 B (**unchanged**) | `irom0`+`drom0` 131396 B → 131460 B (**+64 B**) |

RAM goes *down* everywhere Wi-Fi is on, despite this issue adding several new members
(`wsSeq_`, `lastSendUptimeMs_`, `pendingInitialSnapshot_`, and `broadcastJson_` growing by
`kEnvelopeOverhead` (64 B) to hold the envelope wrapper) - `CONFIG_WEBSOCKET_MAX_CONTEXTS`
dropping from 2 to 1 (the buffer-sharing fix) shrinks Zephyr's own
`contexts[CONFIG_WEBSOCKET_MAX_CONTEXTS]` array by more than that costs. The no-PSRAM config's
RAM is unchanged outright: Wi-Fi is off there, so `QcWifiWebTransport` is never instantiated;
only `QcSession`/`QcMessageTally`'s own small tally-forwarding fix links in, which is flash-only
(a few new branches, no new storage).

### What a second browser sees under the one-client policy

Its WebSocket upgrade completes at the HTTP level (Zephyr has already sent
`101 Switching Protocols` by the point `websocket_register()` fails), so the browser's own
`WebSocket.onopen` fires - immediately followed by `onclose`, since the connection is torn down
right after. The page's existing reconnect loop (`websocket closed - reconnecting…`) then retries
with backoff indefinitely while the first browser stays connected; the second browser can still
load the page itself and its one-time `GET /api/presets.json` fetch (so it isn't blank), it just
never gets live pushes until the first browser disconnects. `Main.cpp`'s log shows only one
`Websocket client connected` line at a time; no corruption, crash or `LOG_ERR` results from the
second attempt.

### Smoke test

1. Flash in station mode, Quad Cortex Mini connected over USB, one browser open on the page.
2. Switch presets on the unit (or change scene/setlist position). Within a couple of seconds,
   confirm both of these:

   ```
   <inf> main: Post-connect tally: command=<N> count=<M>
   <inf> main: Pushed patch to websocket client <fd> (<N> bytes)
   ```

   and the page's status line updating to `preset change (cmd <N>, #<M>)` without a reload.
   Switch presets a second time a few seconds later (well under a minute) and confirm it updates
   again just as quickly - this is the specific case that used to wait up to a minute.
3. Leave the page open and idle (don't touch the unit) for a bit longer than
   `CONFIG_QC_WIFI_WEB_TICK_INTERVAL_MS` (5 s by default). Confirm a `Pushed tick to websocket
   client <fd> (<N> bytes)` line appears and the page doesn't show any error (a tick is silent on
   the page by design).
4. Open a **second** browser tab/window to the same URL while the first is still open. Confirm
   the page loads (title, static listing) but its own console/network tab shows the WebSocket
   opening then immediately closing, retrying on a backoff - matching "What a second browser
   sees" above. The *first* browser's own updates (step 2) must keep working throughout.
5. Close the first browser (or just its tab) and confirm the console shows
   `Websocket client <fd> closed`/similar, then the second browser's next retry succeeds and it
   starts receiving pushes normally.
6. `Post-connect tally: command=33 count=<N>` stepping up roughly once a minute in the log is
   `GlobalTempo` (unchanged from before - that tally log is QcSession's, independent of this
   transport). Confirm it no longer produces a `Pushed patch to websocket client ...` line or a
   page status change on its own - `QcWifiWebTransport::publishPresetChange()` now ignores any
   command that isn't one of the preset-identity ones, so GlobalTempo's own tally bumps reach
   this transport (same as before) but are simply not forwarded to the page at all, which is what
   fixes the old "preset change (cmd 33, ...)" mislabeling.

## Dispatch queue (issue #94)

The owner's hardware run of PR #90 (`53b98ee`, `frdm_rw612`, station mode, 2026-11-11) found
**every preset change on the unit dropping 1-7 USB messages on the dispatch path**, with the
count climbing all session:

```
[00:05:07.547,004] <inf> main: Inflated command 15: 2362 -> 22881 bytes
[00:05:07.569,788] <wrn> main: Traffic drop/overflow: dropped (dispatch=12 gzip=0), ...
[00:05:27.079,123] <inf> main: Inflated command 15: 2005 -> 20036 bytes
[00:05:27.169,812] <wrn> main: Traffic drop/overflow: dropped (dispatch=19 gzip=0), ...
```

### Root cause: the producer outranks the consumer, and the slot was one deep

A preset change makes the device push a burst: a gzipped `RecallPreset` (command 15, inflating
to 14-23 KB) and then `Grid` (1), `SetlistPosition` (2), `Scene` (13), `RecentsFavorites` (20)
and `PresetDirty` (34), on top of `GlobalTempo`'s continuous ~4/s stream. The first four of
those land in `QcDispatcher`'s mailbox, which held exactly one message.

That mailbox can always be beaten, and not because dispatch work is slow. It's a scheduling
fact, read out of the pinned Zephyr v4.5.0 tree:

| Thread | Priority | Preemptible? | Source |
|---|---|---|---|
| `usbh` (calls `submit()`) | `K_PRIO_COOP(9)` = **-7** | **no** | `subsys/usb/host/usbh_core.c:205` |
| `net_tc` rx/tx, HTTP server | `K_PRIO_COOP(15)` = **-1** | **no** | `subsys/net/ip/net_tc.c`, `subsys/net/lib/http/http_server_core.c:38` |
| `qc_heartbeat` | 5 | yes | `QcSession.hpp` |
| `qc_dispatch` | 6 | yes | `QcDispatcher.hpp` |
| `qc_gzip_inflate` | **was 6**, now 7 | yes | `QcGzipInflater.hpp` |
| `qc_session` (handshake) | 7 | yes | `QcSession.hpp` |

The producer is a *cooperative* thread at -7. The scheduler always picks it ahead of any
preemptible thread, and it is never preempted by one. So the consumer only ever runs in the
gaps where `usbh` is blocked on `k_msgq_get()` **and** nothing else is holding the CPU — and
every such stretch that gets stolen is a stretch where the next message in the burst finds the
slot busy and is discarded. Three things steal those stretches, all visible in the owner's log:

1. **The inflate, at the same priority as dispatch.** `gzip::inflate()` on a 14-23 KB payload is
   the one genuinely CPU-bound stretch in this firmware, and it never blocks part-way.
   `qc_gzip_inflate` and `qc_dispatch` both sat at priority 6; Zephyr's default
   `CONFIG_TIMESLICE_SIZE` is **20 ms** (`kernel/Kconfig:591`), longer than a whole inflate, so
   time slicing never broke it up. The consumer could not run *at all* until the inflate
   finished. This is why the drop warning always follows an `Inflated command 15: ...` line by
   20-110 ms in the log — the burst arrived inside that window.
2. **The `Inflated command 15` log line itself.** `CONFIG_LOG_MODE_IMMEDIATE=y`, so that
   `LOG_INF` formats and writes to the UART synchronously on the inflater's own thread, extending
   the same stall by milliseconds.
3. **Wi-Fi.** `net_tc`'s rx/tx threads and Zephyr's HTTP server thread are *also* cooperative
   (priority -1), so while one of them runs, neither `usbh` nor the consumer runs; and when it
   yields, `usbh` at -7 is served before the consumer at 6. More Wi-Fi work overlapping the burst
   means more messages delivered per turn the consumer actually gets.

**This is also the best available explanation for the owner's reconnect observation** — no drop
on the first preset change, drops on every change after the Wi-Fi reconnect. Factor 3 only
applies once a browser is actually connected: after the reconnect the page re-fetched
`/api/presets.json` and re-opened its WebSocket, so from then on every preset change overlapped
real HTTP/WebSocket work on those cooperative threads. **It is not proven from source**, and two
other readings fit the same log equally well: the 00:04:40 change may simply have been a smaller
`RecallPreset` (a shorter inflate, so a shorter stall), or it may have dropped messages that are
already inside the `dispatch=12` the first pasted line shows — that counter was 0 when the File
stream finished, so 12 drops did accumulate earlier in the session, and the pasted excerpt starts
too late to say whether the first change contributed. **What would settle it**: the full console
log from boot (not trimmed), so each `Traffic drop/overflow` line can be matched to the preset
change and the Wi-Fi event before it. The fix below doesn't depend on which reading is right —
it removes the loss for all three factors at once — so this is recorded rather than chased.

### The fix

**A bounded lock-free SPSC queue** ([`QcMessageQueue.hpp`](app/src/QcMessageQueue.hpp)) replaces
the single slot. Deeper queueing is the only structural answer here: raising the consumer's
priority cannot help on its own, because a cooperative producer at -7 outranks any priority a
preemptible consumer can be given.

The depth is affordable because the queue is a **32-entry descriptor ring over one shared byte
arena**, not 32 worst-case buffers. The arena stays at the existing 128 KiB
(`kReassemblyCapacity`/ADR 0007, `CONFIG_QC_MESSAGE_CAPACITY` on the no-PSRAM S3), so the whole
depth costs ~640 bytes of descriptors:

- Records stay **contiguous** in the arena, so the consumer hands a `std::span` straight into it
  to `QcSession::onMessage()` — no second copy, which is what keeps a 128 KiB `File` message
  affordable. A record that would straddle the end is placed at offset 0 and charged the skipped
  tail as padding, freed with it.
- An **empty arena rewinds to offset 0**, so a message needing the entire arena (ADR 0007's
  worst-case fully-named 256-slot folder) still fits wherever the write offset happened to be —
  the single slot always took one, and that mustn't regress.
- Both ends are wait-free; the producer side is one bounded `memcpy` plus two monotonic atomic
  stores, which is all `usbh` is allowed to do (CODING_STANDARDS.md's time-sensitive-code rule
  names a fixed-capacity ring plus a signalling semaphore as exactly the right shape here).

**`QcGzipInflater` drops to priority 7**, below `qc_dispatch`'s 6. The queue bounds the loss;
the priority ordering keeps the queue shallow and, more importantly, stops an inflate from
delaying the preset-*identity* messages by milliseconds — which is what #15 (current preset in
the snapshot) and #78 (confirmed preset switching) need. An inflate has nothing waiting on it
today, so it loses nothing by yielding. It stays above nothing that matters and below
`qc_heartbeat`'s 5, so a stalled inflate still can't delay a KeepAlive (issue #8).

`QcGzipInflater` keeps its **single slot** on purpose: `gzip=0` on every hardware run so far,
including these bursts, because `RecallPreset` is the only commonly compressed message and the
device sends one per preset change, never back to back. If that ever stops being true the
counter says so, and `QcMessageQueue.hpp` is a standalone Zephyr-free template precisely so the
same queue can be dropped in.

What is **not** fixed here: a single `File` message larger than the whole arena still blocks the
next one, exactly as the old slot did. That's issue #66's full plugin listing (113 KB+ single
messages), whose answer is a streaming decoder, not a bigger buffer. `oversizedCount()` now
counts those apart from a transiently full queue so the two can't be confused.

### What the log says now

The drop warning prints the **delta since the previous warning** next to each lifetime total,
instead of leaving the owner to difference `12 → 19 → 24` by hand:

```
<wrn> main: Traffic drop/overflow, new since last warning (total since boot): dispatch=+3 (15, of which oversized 0), gzip=+0 (0), reassembly overflow=+0 (0, largest 0) malformed=+0 (0) - a listing or push in progress may be incomplete
```

and a new line reports peak queue occupancy, **once per increase** — monotonic, so it settles
after the first few bursts rather than logging per message:

```
<inf> main: Dispatch queue high-water: 6 of 32 records, 1536 of 131072 bytes
```

That line is the point of the exercise: with the drops gone, it's the only thing that still
reports anything, and it turns "how big is a real preset-change burst on this device" into a
measured number. If it settles at 6-8 records, 32 has 4x margin; if it ever reaches 32, the
depth is the thing to raise.

### Verified

- `west twister -p qemu_cortex_m3 -p mps2/an385 -K -T firmware/app/tests` — **243/245 cases
  pass**, 1 blocked. The one failure is `firmware.live_updates` timing out on `qemu_cortex_m3`,
  which is pre-existing and expected: that suite's own `testcase.yaml` excludes
  `qemu_cortex_m3` (64 KB RAM, same reason `preset_transport` gives) and an explicit `-p`
  overrides `platform_allow`. Nothing this change touches is compiled into that suite.
- The new `dispatch_queue` suite (11 cases) covers the queue's ordering, arena wrap-around,
  padding accounting, both drop paths, and a full 24-round preset-change burst through the real
  `QcDispatcher` on real Zephyr threads, with the producer at a **cooperative** priority so the
  consumer provably cannot run until it blocks — the same scheduling fact that caused the bug.
  **It fails on the old behaviour**: forcing the depth back to 1 makes it report
  `expected no dispatch drops across 24 preset-change bursts, got 96` — 4 lost per 5-message
  burst, matching the hardware shape — and it passes at depth 32 with zero drops.
- Clean builds, with example credentials in separate build trees (the real
  `app/wifi_credentials.conf` was never read, written or built against): `frdm_rw612` station
  and AP mode, `esp32s3_devkitc/esp32s3/procpu` with and without PSRAM.

RAM/flash deltas against `53b98ee`, same toolchain and same build flags:

| Target | Flash | RAM |
|---|---|---|
| `frdm_rw612`, station | 722304 → 722968 B (**+664**) | 734048 → 734712 B, 74.67% → **74.74%** (**+664**) |
| `frdm_rw612`, AP | 748964 → 749620 B (**+656**) | 741352 → 742016 B, 75.41% → **75.48%** (**+664**) |
| ESP32-S3, PSRAM | 667108 → 667268 B (**+160**) | `ext_dram` 2108720 → 2109376 B (**+656**); `dram0` unchanged |
| ESP32-S3, no PSRAM | 146532 → 146692 B (**+160**) | `dram0` 333424 → 334080 B, 83.54% → **83.71%** (**+656**) |

The RAM cost is the 32 descriptors and a handful of atomics. The message arena itself is
unchanged on every target — including the no-PSRAM S3, where `CONFIG_QC_MESSAGE_CAPACITY`
already shrinks it to 64 KiB and the queue follows it.

**Not verified**: none of this has run on a board.

### Smoke test for this build

Flash in **station** mode with the Quad Cortex Mini connected over USB, and open the page.

1. Boot, USB claim, handshake and the `File` listing as usual. The listing summary now reads
   `File stream limits: max wire=... during this stream: dropped (dispatch=0 gzip=0) ...` —
   both still 0.
2. Shortly after the first messages flow you should see one line like:

   ```
   <inf> main: Dispatch queue high-water: N of 32 records, M of 131072 bytes
   ```

   and then it should repeat only when `N` or `M` grows. **Please paste every one of these
   lines** — `N` is the real burst size, and it's the number the depth of 32 should be judged
   against.
3. **Change the preset on the unit 20+ times in a row**, with the page open, leaving a second or
   two between changes. Expected, per change:

   ```
   <inf> main: Inflated command 15: NNNN -> NNNNN bytes
   <inf> main: Post-connect tally: command=15 count=N
   <inf> main: Pushed patch to websocket client N (NNN bytes)
   ```

   and **no `Traffic drop/overflow` line at all**. That is the pass condition for this issue.
4. **Force a Wi-Fi reconnect in the middle of it** (toggle the AP, or walk out of range and
   back). Expect `Wi-Fi station link dropped and is re-associating ...` then
   `Wi-Fi station connected`, the page recovering on its own, and then another 10+ preset
   changes with still **no** `Traffic drop/overflow` line. This step is the one that reproduced
   the original report, so it's the one that matters most.
5. If a `Traffic drop/overflow` line *does* appear, paste it whole. It now carries the per-event
   delta, the lifetime total, and the `of which oversized` count, and together with the
   high-water line above those four numbers say which of "the burst is bigger than 32", "a
   single message is bigger than the arena" and "something else is stalling the consumer" it
   actually is — which the old line couldn't distinguish.
6. Leave it a few minutes with the page open: no `LOG_ERR` lines, USB/handshake normal,
   `Post-connect tally: command=33 count=<N>` stepping up about once a minute (`GlobalTempo`,
   expected and unchanged).
## Lit web UI (issue #88)

ADR 0011's own research prototype (issue #84, `web/poc/`) is now the board's real UI:
`QcWifiWebTransport.hpp`'s inline `kIndexHtml` test page is gone, replaced by
[`web/ui/`](../web/ui/README.md) - plain ES modules, Lit 3, no build step - served from
flash as build-time, pre-gzipped static resources.

1. **Asset pipeline.** `firmware/app/tools/embed_assets.py` gzips every file in `web/ui/`
   deterministically (mtime 0, so an unchanged UI produces a byte-identical generated
   header) into a `constexpr uint8_t[]` per asset, plus the
   `http_resource_detail_static`/`HTTP_RESOURCE_DEFINE` pair that serves each one on
   `qcWebService` with `Content-Encoding: gzip` and the right `Content-Type`.
   `firmware/app/cmake/EmbedWebAssets.cmake` wires this into both `firmware/app`'s own
   build and `tests/wifi_web_http` (which links the same header) as a CMake custom
   command, depended on by the `app` target - so **editing a file under `web/ui/` needs no
   firmware code change at all**, just a rebuild. Nothing is committed; the generated
   header lives under the build directory.
2. **`GET /api/presets.json` and `/ws` are unchanged** - still hand-written resources in
   `QcWifiWebTransport.hpp`, independent of the embed step. Only `/` moved from the old
   inline page onto the generated static resources (it's just one more entry in the same
   table).
3. **The client store speaks the board's real envelope**, not the richer one `web/poc/`
   proposed: `web/ui/src/store.js` consumes exactly `snapshot`/`patch`/`tick` as
   `QcLiveUpdateEnvelope.hpp` actually emits them (see "Live WebSocket updates" above) -
   there's no "currently loaded preset" on this wire yet, so the UI shows the catalog plus
   the last preset-change event's command/count, same as the inline page it replaces. A
   second browser tab, refused a live WebSocket under the one-client policy, now falls
   back to the same one-shot `GET /api/presets.json` the old inline page made, instead of
   showing a blank list.
4. **Vendored Lit** (`web/ui/vendor/lit.js`, `web/ui/tools/vendor-lit.mjs`) is this tree's
   own pinned bundle, separate from `web/poc/vendor/lit.js` - firmware CI still needs no
   Node; regenerating it is a manual, rarely-run step.

### Parallel connections (PR #92's hardware blocker)

The first build of this UI loaded nothing in Safari, and the owner's curl check from a Mac
showed why: **sequential** requests for every asset returned 200 with the right gzip and
`Content-Type`, but two or more **parallel** connections mostly failed - connection reset
(curl 56), empty reply (52) or refused (7) - and in Safari's own waterfall the parallel
requests (`vendor/lit.js`, `store-controller.js`, `qc-status.js`, `qc-preset-list.js`)
simply hung pending forever, so the module graph never resolved. The old inline page hid
this completely: it only ever made sequential requests on one keep-alive connection.

Two separate limits, both now in `app/prj.conf` with the arithmetic written out there:

1. **`CONFIG_ZVFS_POLL_MAX` was 3** (Zephyr's own default - the larger "6 if
   `WIFI_NM_WPA_SUPPLICANT`" default doesn't apply to the NXP driver). Zephyr's HTTP
   server watches all of its descriptors in one `zsock_poll()` call - 1 eventfd + the
   listening socket + `CONFIG_HTTP_SERVER_MAX_CLIENTS` accepted sockets
   (`subsys/net/lib/http/http_server_core.c`'s `HTTP_SERVER_SOCK_COUNT` and the
   `zsock_poll()` in `http_server_run()`) - and every *active* descriptor needs one entry
   in `zvfs_poll_internal()`'s `struct k_poll_event poll_events[CONFIG_ZVFS_POLL_MAX]`
   (`lib/os/zvfs/zvfs_poll.c`), with `zsock_poll_prepare_ctx()` returning `-ENOMEM` the
   moment it runs out (`subsys/net/lib/sockets/sockets_inet.c`). Three entries is eventfd
   + listener + exactly **one** client. A second simultaneous connection therefore made
   `poll()` fail, and `http_server_run()` treats that as fatal: `close_all_sockets()`
   drops every live connection *and* the listening socket, then the thread re-inits after
   `CONFIG_HTTP_SERVER_RESTART_DELAY` (1 s). Hence resets for connections already open,
   and "connection refused" for anything that connects inside that 1 s window - and, for
   a browser that keeps retrying, a livelock: every retry pair kills the server again.
   `CONFIG_HTTP_SERVER_MAX_CLIENTS=4` was never the effective limit; **1** was.
2. **`CONFIG_HTTP_SERVER_MAX_CLIENTS` was 4**, but a browser opens **six** connections per
   host (Safari's and Chrome's own cap) and keeps them alive. With four client slots the
   other two connections sit unaccepted in the TCP backlog, and since nothing closes a
   keep-alive connection, they wait out
   `CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT` (10 s then, 5 now - see round two below)
   rather than being served. This
   one is independently reproducible: with `ZVFS_POLL_MAX` fixed and `MAX_CLIENTS` left at
   4, `tests/wifi_web_http`'s six-connection test still fails with exactly two of its six
   assets never answered (`errno 11`, nothing received). ADR 0011's assumption that
   over-limit requests would just wait briefly in the TCP backlog holds for one-shot
   requests; it does not hold for a browser's keep-alive connections.

`CONFIG_NET_MAX_CONTEXTS` (6) and `CONFIG_NET_MAX_CONN` (8) were also exactly consumed by
a six-connection load - 1 listening socket + 6 accepted + 1 for the DHCPv4 client - with
nothing left for a connection still in `TIME_WAIT` (`CONFIG_NET_TCP_TIME_WAIT_DELAY` is
1500 ms, and a page reload closes and reopens all six at once). Either pool running empty
sends an RST straight back at the next SYN (`subsys/net/ip/tcp.c`'s `tcp_conn_new()`
failure path calls `net_tcp_reply_rst()`), which is the same curl 56 from a different
cause, so both are raised too.

Off-board coverage, in `tests/wifi_web_http`:
`test_parallel_connections_each_get_their_asset` opens six connections *before* sending
anything and requests one asset on each;
`test_every_page_asset_loads_over_parallel_connections` fetches all nine of the UI's
assets over those six keep-alive connections in two waves, the way the browser does.
Against the pre-fix config the first reports `only 0 of 6 connections were accepted`, the
second `only 3 of 6`, and the server stays down long enough that **every other
socket test in the suite** then fails with `ECONNREFUSED` - 11 of 12 cases red. With the
fix, 12 of 12 pass.

Note that the suite's own `CONFIG_NET_MAX_CONTEXTS`/`NET_MAX_CONN`/`ZVFS_OPEN_MAX` are
deliberately *larger* than the app's: both ends of the loopback connection live in the
same image, so those pools have to hold six client sockets as well as the server's share.
`CONFIG_ZVFS_POLL_MAX` and `CONFIG_HTTP_SERVER_MAX_CLIENTS` do mirror the app exactly -
only the server polls and only the server accepts, so those are the app's real values
under test. **This is why the bug was invisible off-board in the first place**: the suite
had all five raised to the values Zephyr's own `tests/net/lib/http_server/core` uses, so it
was exercising Zephyr's sample limits rather than this firmware's.

The values quoted in this subsection are the ones round one landed on
(`ZVFS_POLL_MAX=10`, `MAX_CLIENTS=6`, `NET_MAX_CONTEXTS=12`, `NET_MAX_CONN=16`); round two
below raises four of them again and explains why. `firmware/app/prj.conf` is always
authoritative.

#### Round two: parked keep-alive connections

The fix above got Chrome loading the page, and the owner's next hardware round showed the
rest of the problem. **Chrome: fine.** **Safari: very slow, with `vendor/lit.js`,
`store-controller.js`, `qc-status.js` and `qc-preset-list.js` still stuck pending** (two of
them eventually completing, two never). The coordinator's parallel curl check, run while
browser tabs were still open, came back *serialised in roughly one-second steps*: 0.115,
0.115, 1.12, 1.16, 2.04, 2.05, 3.07, 3.09, 4.09 s - two connections served per second.

That ladder is **client-side SYN retransmission**, not a timer on the board - no poll
timeout, no `CONFIG_HTTP_SERVER_RESTART_DELAY`, no `k_sleep` of ours is involved. The chain:

1. A browser that has *finished* loading does not close its connections. The server never
   sends `Connection: close` (ADR 0011 noted this), so each of a browser's six connections
   holds an HTTP client slot until `CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT` expires -
   the timer is armed in `init_client_ctx()` and restarted on every data event
   (`http_client_timer_restart()`), so an *idle* connection keeps its slot for the full
   timeout after its last request.
2. While every slot is taken, `handle_listen_pollin()` sets the listening socket's poll
   events to 0 and the server stops accepting at all.
3. Once the listening socket's own TCP backlog fills too, Zephyr stops even completing
   handshakes: `tcp_backlog_is_full()` makes it **drop further SYNs outright** ("same
   behavior as on Linux. Retransmitted SYN attempts may be successful later",
   `subsys/net/ip/tcp.c`).
4. So the client retransmits its SYN about once a second and gets in only as fast as slots
   free up - exactly two per second in the measurement above, which is how many slots were
   actually free behind the open browsers.

Sizing `CONFIG_HTTP_SERVER_MAX_CLIENTS` to "what one page load needs" was the mistake in
round one. The question is **how many connections are still parked when the next page load
starts**: the owner had Chrome and Safari open at once (6 + 6), and a single browser
reloading is the same shape. Chrome got its six in first and loaded fine; Safari queued
behind them. That is the whole Chrome-vs-Safari difference - it is about arrival order and
how many connections are already parked, not about either browser's HTTP implementation.
(The coordinator's probe of six *silent* connections followed by a fast `main.js` fetch is
consistent: connections that never send a request are accepted but their slots were
evidently not all still held when that fetch went out.)

One thing this fix depends on, confirmed in source and now asserted in the suite: **a
WebSocket does not hold an HTTP client slot.**
`handle_http1_to_websocket_upgrade()` calls `websocket_register()` and then
`http_server_release_client()` (`subsys/net/lib/http/http_server_ws.c`), so the upgraded
socket is handed to the application and the slot goes straight back. ADR 0011 claimed this
and it holds. Chrome's open live-update socket is therefore *not* part of the pressure -
only its six idle HTTP connections are. An upgrade does occupy a slot transiently, until
that handoff, which is what the `+1` below is for.

Two settings, and both levers were measured rather than guessed. **The `MAX_CLIENTS=13` in
this table did not survive round three below** - it regressed the board and is now 8; the
inactivity timeout is what carries the multi-browser case. `app/prj.conf` is always
authoritative.

| Setting | Was | Now | Why |
| --- | --- | --- | --- |
| `CONFIG_HTTP_SERVER_MAX_CLIENTS` | 6 | **13** (→ 8, round three) | 6 (first browser) + 6 (second, or the same one reloading) + 1 transient upgrade |
| `CONFIG_HTTP_SERVER_CLIENT_INACTIVITY_TIMEOUT` | 10 | **5** | a parked slot recycles twice as fast - the backstop for a third client |
| `CONFIG_ZVFS_POLL_MAX` | 10 | **16** | 1 + 1 + 13, same arithmetic as above |
| `CONFIG_NET_MAX_CONTEXTS` | 12 | **20** | 1 listener + 13 accepted + DHCPv4, plus `TIME_WAIT` headroom |
| `CONFIG_NET_MAX_CONN` | 16 | **24** | same, one 5-tuple handler per connection |

**Why the timeout isn't cut further, which is the interesting result.** Shortening it alone
(to 2 s, leaving `MAX_CLIENTS` at 6) *does* make the new test pass - but the second
browser's WebSocket still waits a full timeout (2.46 s, versus 0.69 s with 13 slots), so
the page is still visibly slow; and it **breaks
`test_presets_json_on_same_keep_alive_connection` outright**, because the server now drops
the connection in the gap between the page request and the script's own `fetch()` on it.
That is a real keep-alive regression, not a test artifact. Hence slots as the primary fix,
and 5 s (Zephyr's own `HTTP_SERVER_VERSION_3` default, comfortably longer than any gap
inside one page load on a LAN) as the backstop rather than 2 s.

Off-board coverage: `test_a_second_browsers_page_load_is_not_locked_out` parks six
keep-alive connections *and* a WebSocket, then brings a second browser's six connections
and six assets in while all of them are still open. At 6 slots the second browser's
upgrade only gets in after a full 10 s inactivity timeout and the test fails; at 13 it
connects in **0.69 s** with nothing waiting on a timer. (At the 8 slots round three settles
on, it connects after one 5 s recycle instead - see that round's own note on the test's
retuned bar.)

#### Round three: the TCP window is a third of the whole buffer pool, per connection

Round two made hardware **worse**: Chrome went from loading the page to hanging like Safari.
The owner's waterfall on that build served `/` in 3.53 s and the next four files in 11-30 ms
each, then stalled on the parallel wave - `vendor/lit.js` at 20.32 s, `qc-status.js` at
20.00 s, `store-controller.js` at 21.20 s, and `qc-preset-list.js` **failed at 45.18 s with
`net::ERR_CONNECTION_RESET` after "200 OK" and 0.1 kB transferred**. One module failing kills
the whole import graph from `qc-app.js`, so the page never starts. Coordinator probes: single
requests 200 in 0.08-0.11 s, but 9 in parallel gave one 200 and the rest 12 s timeouts, and a
second round of the same probe gave 0 of 9.

"200 OK, then a stall, then a reset mid-response" is not a slot problem - the connection was
accepted and the response had started. And the fact that it got *worse* when
`HTTP_SERVER_MAX_CLIENTS` went 6 → 13 says the binding resource scales with the number of
connections **being served at once**, not with slots.

It is the TCP window, and it is a one-line default in `subsys/net/ip/tcp.c`:

```c
static int tcp_tx_window =
#if (CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE != 0)
        CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE;
#else
        MIN((CONFIG_NET_BUF_TX_COUNT * CONFIG_NET_BUF_DATA_SIZE) / 3, UINT16_MAX);
#endif
```

`CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE` defaults to 0, so the window is **one third of the
entire TX buffer pool - and that value is used per connection** (`tcp_rx_window` is the same
shape out of the RX pool). More than three concurrently-sending connections therefore
oversubscribes the pool **by construction, at any pool size**: raising `NET_BUF_TX_COUNT`
raises the per-connection window by exactly the same factor and changes nothing. The window
has to be pinned.

This board had `NET_BUF_TX_COUNT=36` - Zephyr's own `NET_PKT_LARGE_DEFAULTS` value, which is
`default y if WIFI`, so it was never set by hand - and `NET_BUF_DATA_SIZE=128`. That is a
**1536-byte window per connection against a 4608-byte pool**: 2× oversubscribed at 6 clients,
**4.3× at 13**. TCP holds a segment's buffers until it is ACKed, so on a real Wi-Fi link
(25-150 ms RTT, measured) they stay held for a whole round trip and the pool genuinely runs
dry. The send then stalls; the response ends in a reset rather than an error the client can
retry. And because Zephyr's HTTP server is a **single thread**, a stalled send blocks every
other client too - which is why the whole wave serialises instead of just one request being
slow.

**Why `tests/wifi_web_http` passed this the whole way through, and the lesson from it.** Over
loopback the RTT is microseconds: a buffer is allocated, sent, ACKed and freed before anything
else needs it, so the pool is never actually oversubscribed and the suite passes at any
window size. The suite's buffer counts were never the problem - the *absence of a round trip*
was. That is the one class of bug this suite structurally cannot see, and it is worth
remembering before trusting it about anything throughput- or timing-related: it is a protocol
and resource-accounting harness, not a performance one.

The budget, written out in `app/prj.conf` so it can be checked rather than tuned:

| Setting | Was | Now | Why |
| --- | --- | --- | --- |
| `CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE` | 0 (→ 1536 derived) | **2048** | pinned, so it stops scaling with the pool |
| `CONFIG_NET_TCP_MAX_RECV_WINDOW_SIZE` | 0 (→ 1536 derived) | **2048** | same |
| `CONFIG_NET_BUF_TX_COUNT` | 36 (default) | **160** | 20 480 B = 1.25× the 8 × 2048 B worst case |
| `CONFIG_NET_BUF_RX_COUNT` | 36 (default) | **96** | inbound is request headers and ACKs only |
| `CONFIG_NET_PKT_TX_COUNT` | 16 | **24** | ~2 segments per connection in flight, plus ACKs |
| `CONFIG_NET_PKT_RX_COUNT` | 16 | **24** | same |
| `CONFIG_HTTP_SERVER_MAX_CLIENTS` | 13 | **8** | 6 (browser's cap) + 1 transient upgrade + 1 spare |
| `CONFIG_ZVFS_POLL_MAX` | 16 | **10** | 1 + 1 + 8 |
| `CONFIG_NET_MAX_CONTEXTS` | 20 | **14** | 1 listener + 8 accepted + DHCPv4 + `TIME_WAIT` |
| `CONFIG_NET_MAX_CONN` | 24 | **18** | same |

At a 2048-byte window and a 100 ms RTT that is ~20 KB/s per connection, so the largest asset
(`vendor/lit.js`, 7 053 B gzipped) takes ~0.35 s and a whole six-file wave ~2 s even with the
server serialising them.

**`MAX_CLIENTS` comes back down to 8**, because 13 is what regressed the board. Parked idle
connections cost a slot but no buffers; connections being *served* cost both, and the server
accepts up to `MAX_CLIENTS`, so that number also caps how many sends can be in flight - it has
to stay inside the window budget. Accepting more connections than the link can serve turns a
queue into a stall. Two browsers at once is handled by recycling parked slots quickly (the 5 s
inactivity timeout from round two) rather than by holding a slot open for each, so
`test_a_second_browsers_page_load_is_not_locked_out` now holds the weaker but achievable bar
of "served within one inactivity timeout" (it passes in 6.4 s) instead of "served
immediately".

#### Diagnostics build

Two rounds of this were diagnosed from source plus a loopback suite and then found wrong on
hardware, so `app/diagnostics.conf` exists to make the board report the numbers instead. It is
not part of any normal build:

```bash
west build -b frdm_rw612 firmware/app -d build-diag --pristine \
  -- -DEXTRA_CONF_FILE="wifi_credentials.conf;diagnostics.conf"
```

It turns on `NET_BUF_POOL_USAGE` (per-pool high-water marks and failed-allocation counts - a
pool that ran dry becomes a number instead of a mysterious stall), `NET_STATISTICS_TCP`
(retransmits and segment drops), the net shell (`net mem`, `net stats`, `net conn` on demand),
`THREAD_ANALYZER` plus `STACK_SENTINEL` and `INIT_STACKS` (so a stack overflow is ruled in or
out rather than guessed at - `zvfs_poll_internal()` puts a `k_poll_event[ZVFS_POLL_MAX]` array
on the stack of whichever thread calls `poll()`, and the HTTP server thread's own stack is only
`CONFIG_HTTP_SERVER_STACK_SIZE` = 3 072 B), and the HTTP server's own debug logging. Verified
to build and link: 806 232 B RAM / 82.01% and 1 016 612 B flash on `frdm_rw612` STA.

If a page load still stalls, `net mem` straight afterwards is the single most useful thing to
capture - if the TX pool's high-water mark is at its limit or allocations failed, the window
budget above is still too generous for the link.

#### Cost

Against `940271f` - this branch immediately before the parallel-connection work, rebased onto
the same master (`80d2289`, so #94's dispatch queue is in both) - rebuilt the same way in the
same local environment, with example credentials in a separate build tree:

| Config | RAM/dram0 before → after | Flash before → after |
| --- | --- | --- |
| `frdm_rw612`, STA | 734 712 B / 74.74% → 778 472 B / 79.19% (**+43 760 B**) | 736 748 B → 736 740 B (**-8 B**) |
| `frdm_rw612`, AP | 742 016 B / 75.48% → 785 832 B / 79.94% (**+43 816 B**) | 763 392 B → 763 392 B (**unchanged**) |
| ESP32-S3, with PSRAM | `dram0` 219 504 B / 55.00% → 263 336 B / 65.98% (**+43 832 B**) | `irom0`+`drom0` 1 007 028 B → 1 007 060 B (**+32 B**) |
| ESP32-S3, no PSRAM | `dram0` 334 080 B (**byte-identical**) | **byte-identical** |

Every byte of the `frdm_rw612` STA delta is accounted for, from `nm` on the two
`zephyr.elf`s - and two thirds of it is the buffer pools, which is the point:

| Symbol | Delta | Why |
| --- | --- | --- |
| `net_buf_data_tx_bufs` | +15 872 B | 124 more TX data fragments @ 128 B - `NET_BUF_TX_COUNT` 36 → 160 |
| `net_buf_data_rx_bufs` | +7 680 B | 60 more RX data fragments @ 128 B - `NET_BUF_RX_COUNT` 36 → 96 |
| `server_ctx` | +5 696 B | 4 more `struct http_client_ctx` @ 1 424 B - `MAX_CLIENTS` 4 → 8 |
| `_k_mem_slab_buf_tcp_conns_slab` | +5 504 B | 8 more `struct tcp` @ 688 B - `NET_MAX_CONTEXTS` 6 → 14 |
| `_net_buf_tx_bufs` | +3 472 B | the 124 new TX buffers' own `struct net_buf` headers @ 28 B |
| `_net_buf_rx_bufs` | +1 680 B | the 60 new RX buffers' headers |
| `contexts` | +1 536 B | 8 more `struct net_context` @ 192 B |
| `_k_mem_slab_buf_{tx,rx}_pkts` | +1 216 B | 8 more `struct net_pkt` each @ 76 B - `NET_PKT_*_COUNT` 16 → 24 |
| `conns` | +720 B | 10 more `struct net_conn` @ 72 B - `NET_MAX_CONN` 8 → 18 |
| `fdtable` | +384 B | 8 more fd slots @ 48 B |
| **total** | **+43 760 B** | matches the measured RAM delta exactly |

`CONFIG_ZVFS_POLL_MAX`, the pinned window sizes and the inactivity timeout all cost **no
static RAM**: the first sizes a stack array in whichever thread calls `poll()`, the other
three are plain integers. The no-PSRAM ESP32-S3 config doesn't move because it never enables
networking in the first place (`prj_nopsram.conf`).

ADR 0010's RAM tables are unaffected in their conclusions: the `frdm_rw612` still has ~200 KB
of its 960 KB free (~193 KB in AP mode) and the PSRAM ESP32-S3 ~133 KB of `dram0`. ADR 0011
decision 2's bundler would not have fixed any of this either - one bundle plus the page is
still two parallel requests, two parked keep-alive connections hold slots as long as six, and
a single 7 KB bundle against a 1 536-byte window stalls exactly the same way. It *would* cut
the number of concurrent connections a page load needs, which is worth remembering as a
second-order lever if the window budget above still proves too generous for the link.

### Dev loop

```sh
node web/ui/tools/mock-server.mjs              # http://localhost:8080/
node web/ui/tools/mock-server.mjs --max-ws-clients 1   # exercise the second-tab fallback
```

Edit a file under `web/ui/`, reload - no reflash. `?board=<ip>` points the page's
WebSocket (and its HTTP fallback fetch) at a real board while still serving the UI from
the Mac. See `web/ui/README.md`.

### Size report

```sh
python3 firmware/app/tools/embed_assets.py --ui-dir web/ui --out /tmp/qc_web_assets.hpp
```

Measured 2026-11-10, gzip level 9, 9 assets: **17 096 B total** (vendored Lit 3.3.3 alone:
7 053 B) - under ADR 0011's 20 221 B PoC-era estimate, mostly because this tree ships one
fewer component (no Milestone 3 knob probe) and a 4-symbol Lit bundle instead of 8.

### Verified

`west twister -p qemu_cortex_m3 -p mps2/an385 -K -T firmware/app/tests` (on the
rebase onto `80d2289`): all 24 configurations build, 248 of 250 cases pass, and the only
failure is `firmware.live_updates` on `qemu_cortex_m3`, the same
pre-existing RAM-limitation timeout issue #75's own build already has on that platform
(not caused by this issue; that suite has its own `prj.conf`, untouched by the settings
above, and `mps2/an385` and `native_sim` are what CI actually gates on).
`wifi_web_http` itself: **13/13** cases pass on `mps2/an385` (it's skipped on
`qemu_cortex_m3` for RAM overflow, as `preset_transport` and `dispatch_queue` are).

New Ztest coverage in `tests/wifi_web_http`: the generated asset table is present and
sane (every entry starts with the gzip magic, `/` and `/vendor/lit.js` both present), `/`
is served with `Content-Encoding: gzip` and actually inflates (via
`qcbridge::gzip::inflate()`, the same decoder the firmware uses for compressed device
messages) to the real page, and `/vendor/lit.js` likewise. Plus the three
connection-concurrency tests described under "Parallel connections" above.

Clean builds with example credentials in a separate build tree (this worktree's real
`wifi_credentials.conf` was never created, read or committed), compared against `fd08b49`
(PR #90's tip, current master) rebuilt the same way in the same local environment:

| Config | RAM/dram0 before → after | Flash/(irom0+drom0) before → after |
| --- | --- | --- |
| `frdm_rw612`, STA | 734048 B / 74.67% → 734048 B / 74.67% (**unchanged**) | 722304 B → 736096 B (**+13 792 B**) |
| `frdm_rw612`, AP | 741352 B / 75.41% → 741352 B / 75.41% (**unchanged**) | 748948 B → 762740 B (**+13 792 B**) |
| ESP32-S3, with PSRAM (`CONFIG_BUILD_ONLY_NO_BLOBS=y`, matching CI) | `dram0` 195464 B → 195688 B (**+224 B**) | `irom0`+`drom0` 538276 B → 551844 B (**+13 568 B**) |
| ESP32-S3, no PSRAM | `dram0` 333424 B (unchanged) | `irom0`+`drom0` 131460 B (unchanged) |

RAM barely moves anywhere (the no-PSRAM config not at all: Wi-Fi is off there, so
`QcWifiWebTransport` - and the generated web asset header it `#include`s - is never
compiled in at all). The ~13.6-13.8 KB flash increase on every Wi-Fi-enabled config is
consistent across boards and close to the generated header's own 17 096 B of gzipped
payload minus the inline `kIndexHtml` page it replaced - exactly ADR 0011's own claim that
this decision "costs flash and no RAM" (static resources send straight from flash). The
ESP32-S3 PSRAM baseline measured here (538 276 B) differs slightly from PR #90's own
CI-measured 546 580 B - environment/toolchain drift between this local build and CI's
pinned SDK, confirmed unrelated to this issue by rebuilding `fd08b49` itself locally first
and diffing against that, not against PR #90's own numbers directly. The `frdm_rw612`
numbers, by contrast, reproduce PR #90's CI figures exactly.

### Smoke test (owner - can't run hardware or a browser myself)

1. Flash in either mode. From a Mac on the same network, confirm **parallel** asset
   requests all succeed - this is the check PR #92's blocker failed, and the one a
   sequential check can't catch:

   ```sh
   BOARD=192.168.4.1   # or the station-mode IP
   for p in / /manifest.webmanifest /src/main.js /src/store.js /src/store-controller.js \
            /src/qc-app.js /src/qc-status.js /src/qc-preset-list.js /vendor/lit.js; do
     curl -s -o /dev/null --compressed -w "%{http_code} %{url_effective}\n" "http://$BOARD$p" &
   done; wait
   ```

   All nine lines must read `200`. In round one most came back `000` (curl exit 56/52/7);
   in round two they came back `200` but **serialised in ~1 s steps**, so check the times
   too - every line should land inside a few hundred milliseconds of the others, not in
   pairs a second apart. Run this with **no browser tabs open on the board**, then again
   with one browser tab open, since parked keep-alive connections were the round-two
   cause.
2. **Safari alone, no other browser open.** Quit Chrome (or close its board tabs) first,
   then load the board's URL in Safari: the full preset catalog renders (folders, 48
   presets) and the status line reads "Waiting for a preset change…" until the first one
   arrives. In Web Inspector → Network, all nine files must reach **Completed** - none
   left pending, and no file sitting for seconds. Reload once with the inspector open
   (that closes and reopens a whole browser's worth of connections at once, which
   exercises the `TIME_WAIT` headroom).
3. **Safari with Chrome also open on the board** - this is the round-two case, and the one
   that was broken. Load the board in Chrome first and leave the tab open (it holds six
   keep-alive connections plus the live WebSocket), *then* load it in Safari. Safari must
   still render the full catalog promptly with all nine files Completed. Then reload
   Chrome's tab while Safari stays open, and confirm Chrome recovers the same way. If
   either browser is slow or leaves files pending, grab the board's log for the window -
   `Websocket client ... connected`/`closed` lines and their timestamps show whether
   something is waiting out an inactivity timeout.
4. Switch presets on the unit: within a couple of seconds the status line updates to
   "Preset change (cmd N, #M)" without a reload - same timing bar as issue #75's own smoke
   test (a *second* change a few seconds later must update just as fast, not wait up to a
   minute).
5. On an **iPhone** (Safari), load the same URL, then **Share → Add to Home Screen**.
   Reopen from the home screen icon: confirm it opens `display: standalone` (no Safari
   chrome), the status bar area isn't drawn over by the header, there's no
   double-tap-to-zoom delay on the list, and no rubber-band bounce at the top/bottom of
   the page.
6. Open a **second browser tab/window** to the same URL while the first stays open:
   confirm it shows the full catalog immediately (via its own `GET /api/presets.json`,
   not a blank list) even though its WebSocket never connects (same retry-on-backoff
   behaviour issue #75 already documented above). Close the first tab; the second's next
   retry should connect and start showing live updates.
7. Confirm the page still looks and behaves as before for anything issue #75's own smoke
   test already covered (tick heartbeat with no console error, `GlobalTempo` never showing
   up as a false preset change).

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
