# ADR 0010: Generic transport seam, proven with a Wi-Fi web/WebSocket transport

Date: 2026-11-01
Status: Accepted

## Context

Issue #71 asks whether a web UI served from the board over Wi-Fi is viable enough that the
owner would go with web UI + Wi-Fi instead of the planned JUCE + BLE macOS/iOS app (MVP2).
That decision needs two things settled first: whether `frdm_rw612` can actually run Wi-Fi in
both access-point (AP) and station (STA) mode under Zephyr 4.5, and a transport design that
doesn't lock the firmware into Wi-Fi specifically — the owner wants BLE kept possible later,
either replacing Wi-Fi or running alongside it, without rewriting `QcSession`/`QcPresetCatalog`.

### Does Zephyr 4.5 support Wi-Fi AP and STA on `frdm_rw612`?

Checked directly against the pinned Zephyr v4.5.0 source tree (`~/zephyrproject/zephyr`), not
assumed from general RW612 marketing claims:

- `boards/nxp/frdm_rw612/frdm_rw612_common.dtsi` already enables the SoC's `imu`/`nxp,wifi`
  devicetree node (`&imu { status = "okay"; ... }`) — the integrated Wi-Fi/BLE/802.15.4 radio
  silicon (shared with the sibling RW610 chip) is wired up for this board by default, nothing
  to add in an overlay.
- `drivers/wifi/nxp/Kconfig.nxp`'s `WIFI_NXP` driver (`CONFIG_NXP_RW610` — RW610/RW612 share
  one Kconfig option, distinguished only by flash/pin count, not by the radio) is the one
  Wi-Fi driver path that targets `SOC_SERIES_RW6XX` directly (no `SDHC`/`SDIO_STACK` needed,
  unlike every module-based NXP Wi-Fi part in that same Kconfig — RW612's radio is on-die, not
  attached over SDIO).
- `samples/net/wifi/shell/tests.yaml` explicitly lists `frdm_rw612`/`rd_rw612_bga` under
  `sample.net.wifi.nxp_wifi` and `sample.net.wifi.sta_owe.nxp` — Zephyr's own test plan
  confirms both chips as supported Wi-Fi targets.
- **`samples/net/wifi/apsta_mode` is a dedicated sample for running AP and STA
  *simultaneously*** (`CONFIG_WIFI_USAGE_MODE_STA_AP`), with its own `boards/frdm_rw612.conf`
  — the exact combination issue #71 asks about (AP or STA, build-time choice) is a strict
  subset of what Zephyr's own sample already exercises for this board.
- Real firmware blobs exist for this: `rw61x_sb_wifi_a2.bin` ("WiFi firmware for RW612 A2
  boards") in `hal_nxp`'s blob manifest, fetched the same way as the BLE blobs ADR 0002
  already documents (`west blobs fetch hal_nxp`) — already present on this machine from that
  earlier BLE prep, confirmed by a real, non-`CONFIG_BUILD_ONLY_NO_BLOBS` build linking
  successfully (see Consequences).
- **Correction (ac601b2 hardware results)**: this ADR originally claimed the NXP driver never
  auto-starts a DHCPv4 server for AP mode, and had `QcWifiWebTransport::startAccessPoint()` set
  a static IP and start one itself. That claim was wrong — checked against the generic NXP
  Wi-Fi Kconfig text, not this specific code path. The driver's `CONFIG_NXP_WIFI_SOFTAP_DHCP_SERVER`
  (on by default once SoftAP support is enabled) DOES start its own DHCPv4 server, confirmed at
  `drivers/wifi/nxp/src/nxp_wifi_drv.c`'s `WLAN_REASON_UAP_SUCCESS` handler
  (`net_dhcpv4_server_start(g_uap.netif, &base_addr)`). This firmware's own app-layer server
  start raced it on the same interface (`net_if_get_wifi_sap()` resolves to that same
  `g_uap.netif` — there's only one SoftAP interface on this chip) and always lost with
  `-EALREADY` — the exact `nxp_wifi: DHCP Server start failed` seen on hardware. Worse, because
  the driver's own success handler returns early on that failure, it never reached the lines
  that raise `NET_EVENT_WIFI_AP_ENABLE_RESULT` either, which is why no "AP enabled" log line
  ever followed on hardware. Zephyr's own `apsta_mode` sample independently reaches the same
  conclusion for this exact board: its `frdm_rw612` overlay sets
  `CONFIG_WIFI_SAMPLE_DHCPV4_START=n`, specifically because (per that option's own Kconfig
  help) "some vendors start the DHCPv4 server after Soft AP initialization" — this is that
  vendor. Fixed by removing the app-layer DHCPv4 server entirely and configuring the driver's
  own IP range via `CONFIG_NXP_WIFI_SOFTAP_IP_ADDRESS`/`_MASK`/`_BASE` (prj.conf) instead — see
  `QcWifiWebTransport::startAccessPoint()`'s own comment for the full writeup.

  What this does *not* establish: `WLAN_REASON_UAP_SUCCESS` firing (confirmed by the DHCP
  conflict log existing at all) means the chip firmware itself already reported the SoftAP as
  started *before* hitting this conflict — so whether removing the conflict is sufficient to
  make the AP's SSID visible to a scanning phone (it wasn't, on `ac601b2`) is unconfirmed from
  source alone and needs a fresh hardware run with this fix applied.

Conclusion: **both AP and STA mode are supported on `frdm_rw612` under Zephyr 4.5**, with no
blocking gap — proceeding with the build was the right call, not a workaround.

### The generic transport seam

The owner's hard requirement: `QcSession`/`QcPresetCatalog` must have zero Wi-Fi/HTTP
knowledge, Wi-Fi and a future BLE transport must be able to run side by side or be chosen
independently, and the seam should carry only what a client actually needs (the preset list,
and a change signal) — not HTTP/BLE-specific shapes.

## Decision

1. **`QcPresetTransport`** (`QcPresetTransport.hpp`) is the seam: a pure interface with two
   methods, `publishCatalog(const QcCatalogSnapshot&)` and
   `publishPresetChange(const QcPresetChangeEvent&)`. It depends on nothing but plain C++ and
   `QcCatalogSnapshot.hpp`. `QcSession` depends only on this interface
   (`addTransport(QcPresetTransport&)`, an `etl::vector<QcPresetTransport*, kMaxTransports=2>`
   so Wi-Fi and a future BLE transport can both be registered at once); a concrete transport
   depends on this interface plus whatever its own medium needs, never on `QcSession` or
   `QcPresetCatalog` directly.
2. **`QcCatalogSnapshot`** (`QcCatalogSnapshot.hpp`) is what crosses the seam: a self-contained
   copy of `QcPresetCatalog`'s folders/presets (`copyCatalogSnapshot()`, called under the same
   `fileStreamMutex_` every catalog write already goes through), so a transport never touches
   the catalog's own storage or locking. `QcPresetChangeEvent` carries only a command id and a
   running count — the post-connect tally QcSession already computes (issue #61) — not decoded
   preset content; a transport that needs the new state re-reads the catalog via
   `publishCatalog()`, fired alongside this event for the commands that actually affect the
   preset list.
3. **Call contract**: `QcSession` calls both methods synchronously from its own
   `qc_heartbeat`/`qc_dispatch` threads. An implementation must never block or do real work
   inline — same contract `QcPresetPrinter::requestPrint()` already established — it hands off
   to a thread of its own for anything slower than a copy/enqueue.
4. **`QcWifiWebTransport`** (`QcWifiWebTransport.hpp`) is the first concrete transport:
   Zephyr's HTTP server (`CONFIG_HTTP_SERVER`) serving a small embedded page, `GET
   /api/presets.json`, and a `/ws` WebSocket, on top of Wi-Fi brought up as AP or STA per
   `CONFIG_WIFI_USAGE_MODE_AP`/`_STA` (Kconfig, chosen at build time — see
   `firmware/app/Kconfig`). It's the *only* file in the firmware that mentions Wi-Fi or HTTP;
   `QcSession.hpp`/`QcPresetCatalog.hpp` never include it. `publishCatalog()`/
   `publishPresetChange()` only encode JSON (`QcCatalogJson.hpp`, plain C++, no Zephyr
   dependency, directly unit-tested) and signal a semaphore; the actual socket I/O — which can
   block on a slow or absent link — happens entirely on its own `qc_wifi_web` worker thread.
5. **Credentials**: build-time choice via Kconfig plus a local, gitignored
   `wifi_credentials.conf` (`wifi_credentials.conf.example` committed) — never a runtime
   provisioning flow, and never real credentials in this repo. AP mode defaults to WPA2-PSK
   when a passphrase is set, open otherwise (this build's own default, not something the owner
   asked for — issue #71's own text only says "security can come later"); STA mode skips
   joining rather than attempting a blank SSID. Station mode has no band restriction —
   confirmed by reading `nxp_wifi_connect()` (`drivers/wifi/nxp/src/nxp_wifi_drv.c`): the
   station connect path never reads the connect request's `band` field at all, only
   `channel`/`chan_list`, so an earlier version of this code setting `band = 2.4 GHz` there was
   always a no-op, not an actual restriction. AP mode does stay 2.4 GHz-only, for an honest
   reason that isn't the owner's either: the driver's auto-channel-select only reads that hint
   when picking a 5 GHz channel, and 5 GHz AP operation brings DFS/regulatory channel-vacating
   requirements this firmware does nothing to handle.
6. **The fake-transport seam proof** (`tests/preset_transport`) implements `QcPresetTransport`
   with a plain struct that just counts calls — no Zephyr kernel, no socket, no `QcSession` —
   proving `addTransport()`'s call pattern and `QcCatalogSnapshot`/`QcCatalogJson`'s shape work
   independent of any real transport. See `tests/preset_transport/src/main.cpp`.

## Consequences

- **Go.** The owner has decided Wi-Fi is a viable transport and MVP2 moves to a web UI over
  Wi-Fi, based on the `b735057` hardware result below: station mode on `frdm_rw612`, from a
  Mac, Quad Cortex connected — joined the home network, HTTP server listening, page loaded
  with the full 3-folder/48-preset list, USB/handshake/heartbeat/`File` listing unaffected (21
  messages, no drops), about 3 minutes with no fault. Access point mode came up and a phone
  joined on the prior build (`26bfec1`, before the page-load-crash fix) but wasn't retested
  after it; an iPhone was never tested in either mode; live WebSocket updates don't work and
  are out of scope for this decision, split out to
  [issue #75](https://github.com/rrooding/QCRemote/issues/75). See "Hardware findings
  (`b735057`), and the go decision" below for the full result, and `firmware/README.md`'s
  exact test steps for what remains unconfirmed.
- **RAM/flash cost is real and large** — bringing in Zephyr's networking stack, Wi-Fi driver,
  HTTP server, and mbedTLS (pulled in transitively by the HTTP/TLS-capable build, even with
  plain HTTP in use) costs far more than any feature added so far in this firmware. Measured
  as a clean `frdm_rw612` build against `master` (`6288aea`, unchanged firmware since ADR
  0008's own measurement):

  | Build | Flash | RAM |
  | --- | --- | --- |
  | `master` baseline | 86952 B / 64 MB (0.13%) | 406984 B / 983040 B (41.40%) |
  | This branch, AP mode | 746232 B / 64 MB (1.11%) | 743000 B / 983040 B (75.58%) |
  | This branch, STA mode | 719588 B / 64 MB (1.07%) | 735656 B / 983040 B (74.83%) |

  **Re-measured after the `ac601b2` hardware-fix round** (previously 743200/696104 B AP,
  721860/695248 B STA) — RAM grew further, from moving JSON encoding off `qc_heartbeat` per PR
  #74 review: `QcWifiWebTransport` now also holds a full `QcCatalogSnapshot` copy as member
  storage (~23 KB at this build's `kMaxFolders=8`/`kMaxPresetsPerFolder=64`/`kNameCapacity=32`)
  so the worker thread can re-encode it independently of `publishCatalog()`'s caller, plus a
  third 16 KB JSON buffer (`responseJson_`) so an HTTP GET response survives a concurrent
  broadcast re-encoding `catalogJson_` out from under it. Both are direct, necessary costs of
  fixing real bugs the review found, not scope creep.

  **Re-measured again after the `26bfec1` page-load-crash fix** (`CONFIG_NET_RX_STACK_SIZE`
  1500 B -> 3072 B, see "Hardware findings (`26bfec1`)" below): **AP 744568 B / 75.74% (+1568 B),
  STA 737224 B / 74.99% (+1568 B)**. Trivial and now fully explained — not a repeat of this
  section's earlier open-ended growth, just the direct, measured cost of fixing a confirmed
  stack overflow in Zephyr's own TCP ISN path.

  Flash is trivial against the board's 64 MB external flash either way. RAM is the real cost:
  **+336016 B (AP) / +328672 B (STA)**, roughly **+34 points** now, landing just over 75% of
  the board's 960 KB SRAM. There's still real headroom (240040 B / ~234 KB free at AP, 247384 B
  / ~242 KB at STA, before the board's RAM runs out entirely), but this remains the single
  biggest RAM jump of any issue so far in this project, and grew again this round — the
  owner's go/no-go call should weigh that against the web-UI-viability question this issue is
  answering, not just "does it build."
- **Stack margins** (`-fstack-usage`, same worst-case-call-chain method as ADR 0008, GCC's
  inlining folded trivial wrapper/trampoline calls into their callers at this build's
  optimization level). **Stale as of the `ac601b2` hardware-fix round** — the table below
  predates `run()`/`broadcast()` being restructured, `pollWsClients()` being added, and
  `publishCatalog()` now copying a full `QcCatalogSnapshot` instead of encoding JSON inline;
  it was not re-run with `-fstack-usage` this round (unlike the RAM/flash table above, which
  was). Qualitatively, `qc_wifi_web`'s own added functions (`pollWsClients()`,
  `appendLastChange()`, the two summary loggers) are all small, fixed-size-local functions
  comparable to `broadcast()`'s own frame, and `qc_heartbeat`'s added work
  (`catalogSnapshot_ = snapshot;`, a structural copy through `etl::vector`'s own
  element-by-element assignment, not a stack temporary) is unlikely to add meaningful depth
  given the ~65-95% headroom already measured below — but this is reasoning, not a
  measurement, and should be re-run before treating these specific numbers as current:

  | Thread | Call chain | Budget | Depth | Margin |
  | --- | --- | --- | --- | --- |
  | `qc_wifi_web` (new) | `threadEntryTrampoline()` (`run()` inlined) -> `broadcast()` | 2048 B | 8+88 = 96 B | ~95.3% free |
  | `qc_heartbeat` | unchanged from ADR 0008 (`copyCatalogSnapshot()`/the transport-publish loop added no measurable frame growth) | 2048 B | 8+512+200 = 720 B | ~64.8% free |
  | `qc_dispatch` | unchanged from ADR 0008, byte-for-byte | 2048 B | 32+352+40 = 424 B | ~79.3% free |
  | `main` (boot-time `start()` -> `startAccessPoint()`, once) | 88+80 = 168 B | 4096 B | ~95.9% free |

  `qc_heartbeat`'s and `qc_dispatch`'s own numbers matching ADR 0008's measurement exactly
  confirms the new per-transport publish loop (a 0-2-element pointer loop, no stack-local
  temporaries — `catalogSnapshot_` is `QcSession`'s own member storage, same "big objects never
  go on a thread's stack" rule the catalog itself already follows) added no measurable stack
  cost to either thread. `qc_wifi_web` itself has by far the most headroom of any thread in
  this firmware — `broadcast()`'s own locals are a couple of small `etl::vector`s, not a
  preset listing. (Not included: however deep Zephyr's own `websocket_send_msg()`/socket-layer
  internals go on `qc_wifi_web`'s stack past our own code, same boundary ADR 0008's own tables
  drew at `sendMessage()`'s call into `usbh_shim.c`.)
- **No logging, no catalog work, nothing new on `usbh`** — this issue's changes touch
  `QcSession.hpp`'s heartbeat/dispatcher-thread paths and add one new thread entirely; `usbh`
  (1536 B stack, `CONFIG_LOG_MODE_IMMEDIATE`) is untouched.
- **BLE isn't built** — `kMaxTransports=2` and the interface design leave room for it, and
  `tests/preset_transport`'s fake transport is the proof the seam itself doesn't need Wi-Fi to
  exercise, but no BLE transport exists yet. That's issue #71's explicit scope boundary, not an
  oversight.
- **ESP32 carryover**: ADR 0002 is updated with a dated note — the board this firmware might
  move to later (if it ever needs both USB host and Wi-Fi in one chip) is **ESP32-S3**, not the
  ESP32-C3 that ADR 0002's own "Decision" section named, since the C3 has no USB-OTG/host
  silicon at all (confirmed against Espressif's docs, flagged there for confirmation against
  the S3's own datasheet before any real board-switch decision).
- **Security is deliberately out of scope here** — no auth on the HTTP/WebSocket endpoints and
  plain HTTP (not HTTPS), matching issue #71's own stated scope ("home use, not gigging...
  security can come later"). WPA2-by-default on the Wi-Fi link itself (when a passphrase is
  configured) is this build's own choice, not something the issue or the owner asked for
  specifically — open-by-default would have been equally in scope. Revisit if this graduates
  past a test build.

## Hardware findings (`ac601b2`)

Both modes built and flashed; USB, the 200ms KeepAlive, and the `File` listing (21 messages, no
drops, full 48-preset list, handshake 4.1.1) kept working with Wi-Fi active in both modes — the
coexistence requirement held. Wi-Fi itself failed in both modes. Findings below are split by
how they were established — source-confirmed causes already fixed in this round, vs. what's
still unconfirmed and needs another hardware run.

**AP mode — confirmed from source, fixed**: `nxp_wifi: DHCP Server start failed` on the serial
console, with no "AP enabled" line following. Root cause confirmed by reading
`drivers/wifi/nxp/src/nxp_wifi_drv.c` (see the DHCP correction above): this firmware's own
app-layer DHCPv4 server start raced the NXP driver's own, lost with `-EALREADY`, and the
driver's own early return on that failure is why the enable-result event (and therefore the
"AP enabled" log line) never fired. Fixed by removing the app-layer server entirely.
**Unconfirmed**: whether this fix alone makes the AP's SSID visible to a scanning phone — on
`ac601b2` the owner reported "QCRemote" never appeared at all, and `WLAN_REASON_UAP_SUCCESS`
firing (which the DHCP conflict log proves happened) means the chip firmware had already
reported the SoftAP as started *before* hitting that conflict. So the DHCP fix addresses a
real, confirmed bug, but is not proven sufficient on its own to explain full AP visibility —
needs a fresh hardware run to tell apart from a separate, still-unidentified cause (channel
selection, a regulatory/country-code setting this driver doesn't expose a Kconfig for, or
something else at the radio level).

**STA mode — confirmed from source, fixed**: the serial console showed `Wi-Fi station
connected` printed immediately around a logged `authentication failed` warning, and the board
never appeared in the router's DHCP client list. Root cause confirmed by reading the same
driver file: `onWifiEvent()`'s `NET_EVENT_WIFI_CONNECT_RESULT` handler logged "connected"
unconditionally, never reading the event's own `wifi_status`. The driver raises that exact
event with a non-zero status (`WIFI_STATUS_CONN_WRONG_PASSWORD`, the driver's own generic code
for *any* 4-way-handshake failure, not only an actually-wrong passphrase) on
`WLAN_REASON_NETWORK_AUTH_FAILED`, so the "connected" log line next to an "authentication
failed" warning was this firmware misreporting a failed (re)association as a success, not two
real but contradictory events. Fixed by reading `status->conn_status`/`status_code`/
`reason_code` and logging success/failure accurately. The station connect path was also
confirmed to never read the connect request's `band` field at all (only `channel`/
`chan_list`), so the previous `WIFI_FREQ_BAND_2_4_GHZ` hardcode was already a no-op for station
mode on this driver — removed for accuracy, not because it was causing the failure.
**Unconfirmed**: why the handshake fails repeatedly in the first place. The owner has since
confirmed the "Radioshack" network is WPA2 (not WPA3 or mixed) on both 2.4 GHz and 5 GHz under
the same SSID, which rules out a security-type mismatch — this build's `WIFI_SECURITY_TYPE_PSK`
request was already the correct type for that network. Possible remaining causes not yet
confirmed from source: a management-frame-protection (MFP) mismatch (this code leaves
`wifi_connect_req_params::mfp` at its zero-initialized `WIFI_MFP_DISABLE`; the router's actual
PMF setting is unknown), or something at the radio/firmware level shared with the AP-mode
failure (see below). Needs another hardware run with the improved logging to see the actual
status/reason codes.

**Shared-cause check**: both modes failing at the radio level (not just our own app code) was
checked against Zephyr's own `samples/net/wifi/apsta_mode`/`shell` samples for this exact
board — no missing Kconfig option, country/regulatory-code setting (this driver exposes none
for `frdm_rw612`), or driver option was found that this firmware's `prj.conf` was missing. The
`sections-rom.ld` linker fragment this issue added is a standard per-service iterable-section
definition (same pattern as `samples/net/sockets/http_server`'s own) with no overlap with
anything USB-related. Nothing in source review points at USB host activity or the linker change
as a plausible shared cause; this is not the same as proof, since the actual radio-level
failure mode is still unconfirmed.

**Other hardware observations, acted on**: `thread_analyzer`'s own worker peaked at 952/1024
bytes (92%) in this build — `CONFIG_THREAD_ANALYZER_AUTO_STACK_SIZE` raised back to 2048 (the
value earlier builds used, per ADR 0007/0008) rather than leaving it this tight.
`rx_q[0]` (936/1504, ~62% used) and `conn_mgr_monitor` (296/512, ~58% used) both still have
healthy margin — read as sane, no change made.

## Hardware findings (`26bfec1`), and the page-load crash

Both modes came up this round: AP enabled at `192.168.4.1` with a station joining, and STA
connected with a real DHCP-assigned IP — the DHCP-conflict and connect-logging fixes above both
held. Loading the page then crashed identically in both modes: a USAGE FAULT ("Stack overflow
(context area not valid)") on thread `rx_q[0]`, same faulting PC (`0x18006dba`) and LR
(`0x18004db5`) in both logs. Neither log showed a Quad Cortex claimed; the owner has since
confirmed it was powered off for both runs, so this is not a USB- or catalog-size-dependent bug.

**Root cause — confirmed from source and from this build's own `zephyr.elf`, not our own code.**
`addr2line` against a from-source rebuild of `26bfec1` (CI's `d44d3fc` on top doesn't touch
firmware) resolves the fault: PC is the prologue of `mcuxClMemory_copy` (NXP's hal_nxp hash
library), LR is inside `mcuxClHashModes_Els_oneShot_Sha2_Padding`. Tracing callers through the
same `zephyr.elf` (`objdump -d`, following each `bl` target) finds exactly two call sites that
reach this chain anywhere in the image:

- `subsys/net/ip/tcp.c`'s `tcp_isn_hash()`, called inline from TCP SYN processing — confirmed
  this runs on `net_tc`'s own `rx_q[N]` thread at `net_tc.c:421` (`snprintk(name, ..., "rx_q[%d]",
  i)` is literally where that thread gets its name).
- Zephyr's own `handle_http1_to_websocket_upgrade()` (the Sec-WebSocket-Accept hash), which runs
  on the HTTP server's own thread, not `rx_q[0]` — not the match for this fault, but the same
  underlying library call explains why both paths share a crypto chain.

`tcp_isn_hash()` is the match: `subsys/net/ip/Kconfig.tcp`'s `CONFIG_NET_TCP_ISN_RFC6528`
(`default y`, `depends on PSA_WANT_ALG_SHA_256`) is off on `master` today — nothing there
satisfies that dependency — and turns on as a side effect of this issue enabling Wi-Fi, which
pulls in `CONFIG_PSA_CRYPTO`/`CONFIG_MCUX_ELS_PKC` for the Wi-Fi driver's own security needs.
Once on, computing a TCP initial sequence number for *every newly-accepted connection* switches
from `master`'s shallow `sys_rand32_get()` fallback to a real `psa_hash_compute(PSA_ALG_SHA_256,
...)` call, routed through `psa_driver_wrapper_hash_compute()` -> Zephyr's `els_pkc` PSA crypto
driver -> `mcuxClPsaDriver_psa_driver_wrapper_hash_compute()` -> `mcuxClHash_compute()` ->
`mcuxClHashModes_Els_oneShot_Sha2()` and its Padding/FullBlocks/ProcessData/Truncated helpers ->
`mcuxClMemory_copy()` (where it faults). Measuring each frame the same way as every other
stack-margin table in this ADR (gcc's prologue `push`/`stmdb` + `sub sp`, read directly from
`objdump -d` on this exact build): `tcp_isn_hash` 56 B, `psa_hash_compute` 0 B net (it pops its
own 8 B before tail-calling, confirmed by its own `pop {r4,r5}; b.w` sequence),
`psa_driver_wrapper_hash_compute` 40 B, `els_pkc_transparent_hash_compute` 32 B,
`mcuxClPsaDriver_psa_driver_wrapper_hash_compute` 296 B, `mcuxClHash_compute` 120 B,
`mcuxClHashModes_Els_oneShot_Sha2` 152 B, its data-processing callee 108-240 B depending on input
length, `mcuxClHashModes_Els_oneShot_Sha2_Padding` 120 B, `mcuxClMemory_copy` 80 B — **~1136 B for
this chain alone**, before whatever depth `net_tc`/IP/TCP processing itself already needed to
reach SYN handling. `CONFIG_NET_RX_STACK_SIZE`'s default (1500 B, matching the "1504" budget
`thread_analyzer` reported pre-crash) was sized for the pre-Wi-Fi, non-cryptographic ISN path and
was never going to survive this chain once Wi-Fi turned it on. This also explains every detail of
the report: identical PC/LR in both modes (the chain is mode-independent — any new TCP
connection triggers it), the crash tied to "loading the page" (the browser's first GET/WebSocket
request is the first new TCP connection), and an empty catalog with no Quad Cortex attached still
crashing (this path never touches our own catalog/HTTP/JSON code at all — it runs before any of
that gets a chance to).

**Fix**: `CONFIG_NET_RX_STACK_SIZE` raised from its 1500 B default to 3072 B (`prj.conf`, with
the measurement above recorded in a comment there). Re-verified with a from-source rebuild of
this exact fix for both modes: clean links, RAM `+1568 B` in each mode (3072 B rounds the
underlying `K_KERNEL_STACK_ARRAY_DEFINE` allocation up slightly past the raw 1572 B difference) —
a trivial, now-known cost, not a repeat of the open-ended growth flagged earlier in this ADR.
**Confirmed fixed on hardware** — see "Hardware findings (`b735057`), and the go decision" below.

**Other thread margins, checked, no change made**: the pre-crash `thread_analyzer` snapshot also
showed `net_socket_service` at 2288/4096 (55.8%) and `sysworkq` at 1432/2048 (69.9%) — both
higher than `rx_q[0]`'s own pre-crash 408/1504, but neither anywhere near its budget, and neither
is on the path identified above (the HTTP server and websocket worker each run on their own
thread, not `sysworkq`; `net_mgmt` events already get their own 4608 B thread, not routed through
`sysworkq` either). No source-confirmed reason to raise either found - read as sane, no change
made, same as `conn_mgr_monitor` and `rx_q[0]` itself were in the `ac601b2` round above.

## Hardware findings (`b735057`), and the go decision

Station mode, from a Mac, Quad Cortex Mini connected: joined the home network and got an IP
from DHCP; the HTTP server was listening. The page loaded with the full catalog — 3 folders,
48 presets. USB, the handshake (`4.1.1`), the heartbeat, and the `File` listing were
unaffected: 21 messages, no drops or overflows. A preset change arrived over USB as before
(`RecallPreset` inflated 1223 → 13235 bytes). About 3 minutes with no fault; peak stacks
`rx_q[0]` 1612/3072, `http_server_tid` 1528/3072, `usbh_bus` 1000/1536 — confirming the
`CONFIG_NET_RX_STACK_SIZE` fix above with real margin to spare, not just enough to survive.

This is the result the owner's go decision is based on: **Wi-Fi is a viable transport, and
MVP2 moves to a web UI over Wi-Fi.**

**What this run does not cover**, and so remains exactly as unconfirmed as the sections
above left it:

- **Access point mode on this build.** AP mode last came up on `26bfec1` (a phone joined
  "QCRemote", got an IP) — that was *before* the `rx_q[0]` page-load-crash fix this build
  carries, so AP mode's own page load was never retried against it. Nothing points at an
  AP-specific gap (the crash was mode-independent — see the `26bfec1` findings above), but it
  is untested, not assumed-fine.
- **An iPhone**, in either mode. Every hardware result in this ADR is from a Mac (or, for the
  `26bfec1` AP join, unspecified which device joined). Safari/iOS's networking stack hasn't
  touched this build at all.
- **The 10-15 minute soak** `firmware/README.md`'s test steps ask for — this run was ~3
  minutes, long enough to rule out the specific crash this round fixed, not a longer-duration
  regression.
- **Live WebSocket updates.** These don't work on this build. The owner has treated that as
  out of scope for the go/no-go decision above, and it's split out to its own issue
  ([#75](https://github.com/rrooding/QCRemote/issues/75)) rather than blocking this one. A
  client still gets the catalog on page load (`GET /api/presets.json`); it just doesn't update
  live without a manual reload. **Corrected after the `5068403` smoke test**: that last sentence
  was wrong. `GET /api/presets.json` answered with a malformed status line and no browser ever
  accepted it — this run's page got its listing over the WebSocket, because here the catalog
  happened to be published after the browser had already connected. See the `5068403` findings
  below.

**ESP32-S3**: Wi-Fi was also wired up for the ESP32-S3 target in this round (issues #71/#76,
after rebasing onto [#77](https://github.com/rrooding/QCRemote/issues/77)) — see
[ADR 0009](0009-esp32-s3-second-target.md) and `firmware/README.md`'s "ESP32-S3 target"
section. That's a CI-only build-cleanliness check so far (`CONFIG_BUILD_ONLY_NO_BLOBS`, no
Espressif Wi-Fi blobs fetched), not a hardware result — none of the findings in this ADR have
been run against the S3 with Wi-Fi actually enabled. The S3's own hardware verification to
date (issue #76/PR #77) is USB-only, with Wi-Fi off (the no-PSRAM config).

## Hardware findings (`5068403`), and two bugs the go-decision run hid

The owner's smoke test of `5068403` (station mode, Quad Cortex Mini connected) found USB, the
handshake and the `File` listing fine — 21 messages, no drops, 3 folders / 49 presets — and two
problems. Both are now reproduced off-board and fixed; one of them had been there all along and
only looked like a regression.

### 1. The page stayed on "Loading…" — a malformed status line, plus no recovery path

Zephyr's HTTP server builds a **dynamic** resource's response line from
`RESPONSE_TEMPLATE_DYNAMIC_PART1` in `subsys/net/lib/http/http_server_http1.c`:

```c
#define RESPONSE_TEMPLATE_DYNAMIC_PART1 "HTTP/1.1 %d%s%s\r\n" "Transfer-Encoding: chunked\r\n"
```

Both `%s` are empty unless `CONFIG_HTTP_SERVER_COMPLETE_STATUS_PHRASES` is on (it defaults off),
so `GET /api/presets.json` answered with a status line of `HTTP/1.1 200\r\n` — missing not just
the reason phrase but **the space after the status code**, which RFC 9112 §4 makes mandatory
(`status-line = HTTP-version SP status-code SP [ reason-phrase ]`; only the phrase itself is
optional). Zephyr's own Kconfig help for that option says the absence "does not affect protocol
compliance or client behavior", which the line it generates contradicts.

The embedded page is a *static* resource, and `handle_http1_static_resource()` sends a hardcoded
`HTTP/1.1 200 OK` with a real `Content-Length` — so the page always loaded while its own
`fetch('/api/presets.json')` was the one request in this firmware not sending a well-formed
status line. `CONFIG_HTTP_SERVER_COMPLETE_STATUS_PHRASES=y` (`prj.conf`) fixes it.

This is reproduced and pinned by a new Ztest suite, `firmware/app/tests/wifi_web_http`, which
drives the real `QcWifiWebTransport` resources over a real TCP socket on Zephyr's loopback
interface — the browser's own sequence (`GET /`, then `GET /api/presets.json` both on the same
keep-alive connection and on a fresh one, then a WebSocket upgrade), with real Safari request
headers. It fails without the option and passes with it. No Wi-Fi driver and no board are needed:
`QcWifiWebTransport::startServer()` is the HTTP/WebSocket half of `start()`, split out for this.

Two things compounded it, and are fixed too:

- **A newly connected WebSocket client received nothing at all until the *next* publish.**
  `publishCatalog()` fires once per device connect (when the `File` stream finishes) and
  `publishPresetChange()` is throttled to one push per minute per command, so a page opened after
  the catalog was already published sat empty until the next throttled push. `handleWsSetup()` now
  wakes the worker so the client gets the catalog the firmware already holds immediately.

  This also explains why the `b735057` go-decision run looked fine: there, the catalog was
  published *after* the browser had connected, so the WebSocket delivered it. On `5068403` the
  order flipped — the catalog is published at 12.59 s and Wi-Fi only connects at 13.04 s (the
  `File` READ now waits for the state-READ replies to drain, `QcSession::waitForInTrafficToSettle()`,
  issue #76) — so a page opened afterwards had nothing coming. The claim in the `b735057` section
  above that "a client still gets the catalog on page load (`GET /api/presets.json`)" was wrong:
  that fetch has never worked in a browser.

- **The page had no error path and no retry.** It fetched once, with no `.catch`, and opened one
  WebSocket with no `onclose` — so a single failed request left it reading "loading…" with nothing
  on screen or in the log to say why. It now reports what failed in its own status line, retries
  the fetch, and reconnects the WebSocket. The firmware also logs every `GET /api/presets.json` it
  answers and every WebSocket push, so "the browser never asked" and "the firmware answered and the
  browser rejected it" are distinguishable from the console next time.

`catalogJson_` is also now re-encoded by `handlePresetsGet()` if the snapshot is newer than the
JSON, not only by the worker thread's `broadcast()` — a `GET` can no longer serve a stale listing
because of thread ordering. (Still off `qc_heartbeat`, which is what PR #74's review asked for:
this runs on Zephyr's own single HTTP server thread.)

### 2. "Wrong password" ~52 s into a working session — the vendor stack's misreport

`nxp_wifi: WLAN: nxp_wlan_network authentication failed` / `Wi-Fi station connect failed: Wrong
password (status_code=0, reason_code=0)` appeared 52 s after a successful connect, on a correct
passphrase, and the link came back 3.4 s later. Traced through the vendor stack:

- `wlcm_process_disassoc_event()` (`modules/hal/nxp/mcux/middleware/wifi_nxp/wlcmgr/wlan.c`)
  handles *any* asynchronous disassociation from the AP by calling
  `do_connect_failed(WLAN_REASON_NETWORK_AUTH_FAILED)` **unconditionally** — it never looks at the
  802.11 reason code. The link-loss path does the same for every reason other than a
  4-way-handshake timeout.
- Zephyr's driver then maps that single `WLAN_REASON` value to one status:
  `wifi_mgmt_raise_connect_result_event(g_mlan.netif, WIFI_STATUS_CONN_WRONG_PASSWORD)`
  (`drivers/wifi/nxp/src/nxp_wifi_drv.c`), whose text is "Wrong password".
- `wlan.reassoc_control` then re-associates on its own, which is the `Wi-Fi station connected`
  line that follows (and the `board_type: 2` print in between, from the middleware's own
  `wlan_txpwrlimit_cfg.c` on the re-association path).

So the status covers an AP-initiated disassoc/deauth, a lost EAPoL frame and an actually-wrong
passphrase alike. Nothing in this firmware is wrong, and there is nothing here to fix beyond the
log line, which now says what actually happened: a station that has associated successfully at
least once logs a dropped-link-and-re-associating warning naming the vendor status and stating
plainly that it is not evidence of a wrong passphrase; a station that has never associated still
gets the "check your SSID/PSK" wording. `status_code`/`reason_code` are no longer printed on that
path — `wifi_mgmt_raise_connect_result_event()` (`subsys/net/l2/wifi/wifi_mgmt.c`) fills in only
`.status` and leaves both at 0, and this driver never uses the `_status_event()` variant that
could set them, so `status_code=0, reason_code=0` was structurally guaranteed and carried no
information.

**What the underlying 802.11 reason was is still unknown** — it needs
`CONFIG_WIFI_LOG_LEVEL_DBG`, which turns on the middleware's own
`wlcm_d("got event: disassociation, code=%d", ...)`. It isn't needed to explain the log line, and
the link recovers on its own, so this build doesn't enable it.

### 3. `Post-connect tally: command=33 count=2` → `count=170`: expected

Command 33 is `GlobalTempo` (`QcStateReads.hpp`), which `docs/PROTOCOL-NOTES.md` already records
as arriving continuously at about 4/s for the whole session once the heartbeat is active, with or
without any preset change. 168 more in 60 s is 2.8/s — that behaviour, nothing else. The tally
logs an entry at most once per minute, which is why it steps rather than streams.

One real wart falls out of it: because a bumped tally entry is what gets forwarded to transports,
a `GlobalTempo` tick pushes a `lastChange` of `{"command":33,...}` and the page's status line calls
it a "preset change". That is misleading rather than broken, and belongs with the message schema
([#15](https://github.com/rrooding/QCRemote/issues/15)) and live preset-change updates
([#75](https://github.com/rrooding/QCRemote/issues/75)), not here.

### Still open after this round

- **The shared WebSocket receive buffer** (ADR 0011, PR #85). `websocket_register()` stores the
  caller's buffer pointer per context (`ctx->recv_buf.buf = recv_buf`) while keeping each
  context's own `count`/`parser_state`, and Zephyr's server passes the resource's single
  `data_buffer` for every upgrade — so with `CONFIG_WEBSOCKET_MAX_CONTEXTS=2` two simultaneous
  clients share `g_qcWsDataBuffer[256]` and can corrupt each other's partially received frames.
  Confirmed, but **not** part of the "Loading…" bug (one browser, one WebSocket), and not fixable
  within Zephyr's API without dropping to a single client — left to
  [#75](https://github.com/rrooding/QCRemote/issues/75).
- Everything the `b735057` section lists as uncovered: AP mode on a current build, an iPhone, and
  a 10-15 minute soak.
