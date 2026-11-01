# ADR 0009: Generic transport seam, proven with a Wi-Fi web/WebSocket transport

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

- **This is a go, pending hardware confirmation.** Both modes build clean on `frdm_rw612`
  (measurements below); the actual AP/STA/web-UI behavior on real hardware is unverified — see
  `firmware/README.md`'s exact test steps for each mode.
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
