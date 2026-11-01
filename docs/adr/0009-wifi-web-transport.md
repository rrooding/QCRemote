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
- One real gap found and fixed along the way, not assumed from the sample: the NXP driver
  never auto-starts a DHCPv4 *server* for AP mode (only `CONFIG_WIFI_STA_AUTO_DHCPV4`'s
  *client* path exists, confirmed against `drivers/wifi/nxp/src/nxp_wifi_net.c`) — a joining
  phone/laptop would get no IP address otherwise. `QcWifiWebTransport::startAccessPoint()`
  sets a static IP and starts a server itself, the same pattern `apsta_mode`'s own
  `enable_dhcpv4_server()` uses.

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
   when a passphrase is set (the owner's decision — security hardening deferred, but not to an
   open network by default); STA mode skips joining rather than attempting a blank SSID.
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
  | This branch, AP mode | 743200 B / 64 MB (1.11%) | 696104 B / 983040 B (70.81%) |
  | This branch, STA mode | 721860 B / 64 MB (1.08%) | 695248 B / 983040 B (70.72%) |

  Flash is trivial against the board's 64 MB external flash either way. RAM is the real cost:
  **+289120 B (AP) / +288264 B (STA)**, roughly **+29 points**, landing just over 70% of the
  board's 960 KB SRAM. There's still real headroom (286936 B / ~280 KB free at AP, 287792 B at
  STA, before the board's RAM runs out entirely), but this is the single biggest RAM jump of
  any issue so far in this project — the owner's go/no-go call should weigh that against the
  web-UI-viability question this issue is answering, not just "does it build."
- **Stack margins** (`-fstack-usage`, same worst-case-call-chain method as ADR 0008, GCC's
  inlining folded trivial wrapper/trampoline calls into their callers at this build's
  optimization level):

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
- **Security is deliberately out of scope here** — no auth on the HTTP/WebSocket endpoints,
  per the owner's decision (home use, not gigging; WPA2 on the Wi-Fi link itself is the one
  hardening applied). Revisit if this graduates past a test build.
