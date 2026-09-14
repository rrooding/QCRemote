# ADR 0002: Use NXP FRDM-RW612 for firmware development

Date: 2026-09-14
Status: Accepted

## Context

The firmware needs to (a) act as a USB host to the Quad Cortex Mini and speak its
vendor-specific HID protocol (qc-mcp's reverse-engineered framing, session handshake, and
message set — see [research report](https://github.com/rrooding/QCRemote), the earlier
research conversation), and (b) run a BLE peripheral for the macOS/iOS app, on Zephyr with
C++23 (per [CODING_STANDARDS.md](../../CODING_STANDARDS.md)).

Candidate devkits on hand: NXP FRDM-RW612, NXP MCXN947, NXP FRDM-i.MX91, STM32H743,
ESP32-S3-WROOM, ESP32 DevKitV1 (classic ESP32). A further switch to ESP32-C3 is anticipated
later, once a smaller final-form-factor board is chosen.

Key constraint discovered during evaluation: Zephyr's USB host stack (UHC) is broadly
early/experimental project-wide, regardless of which SoC is chosen — this isn't a gap
specific to any one candidate, and the earlier qc-mcp-based research had already flagged
"Zephyr USB Host is immature" as a general risk versus ESP-IDF. Given C++23-across-the-board
was already decided (see CODING_STANDARDS.md), the question became which available devkit
has the *most mature* Zephyr USB **host** support today, since that's the highest-risk,
least-mature part of the stack:

- **NXP MCXN947**: USB HS Host/Device hardware with on-chip PHY and a merged controller
  driver, but no onboard BLE radio (would need an external module) and no onboard evidence
  of host-stack maturity beyond the base controller driver.
- **NXP FRDM-i.MX91**: Cortex-A55 application processor aimed at Linux — the wrong tier
  entirely for a lean Zephyr firmware image, and no onboard BLE either. Ruled out.
- **STM32H743**: no onboard BLE, and not among the SoC families Zephyr's DWC2 host driver
  currently lists as supported (F7/U5 only, not H7). Ruled out.
- **ESP32-S3-WROOM**: onboard BLE 5.0, and a merged DWC2-based UHC driver — but that driver
  is explicitly "initial support" with a known open bug (concurrent BULK transfers don't
  work).
- **ESP32 DevKitV1**: no native USB peripheral at all (UART-only USB-serial bridge), so it
  cannot do USB host without an external host controller chip regardless of RTOS. Weaker,
  older BLE (4.2) too.
- **NXP FRDM-RW612**: onboard BLE 5.3 + Wi-Fi 6, 1.2MB SRAM + 64MB external flash (most
  generous budget of any candidate), and its low-level USB host transport driver
  (`drivers/usb/uhc/uhc_mcux_ehci.c`, `CONFIG_UHC_NXP_EHCI`) is **merged in mainline
  Zephyr** — more mature than the ESP32-S3's DWC2 UHC driver. NXP is actively building
  host-side class drivers (Mass Storage, USB Audio Class 2.0) on top of this same EHCI
  driver as of this writing, which is real-world exercise of exactly the transport layer
  our own vendor-HID protocol code will sit on. Those class drivers themselves are irrelevant
  to us (the Quad Cortex Mini isn't a mass-storage or audio device — we're writing our own
  protocol layer on top of raw control/interrupt transfers either way), but their existence
  demonstrates the underlying driver handles more than trivial traffic.
  - An earlier concern (an open bug disabling the USB clock, zephyr#88304) turned out on
    closer reading to be about the **device**-mode controller (UDC) under the newer
    `usb_next` stack, not host mode — and it's already fixed
    ([zephyr#86409](https://github.com/zephyrproject-rtos/zephyr/issues/86409) →
    [PR #88165](https://github.com/zephyrproject-rtos/zephyr/pull/88165), merged April 2025).
    Not a live blocker either way.

## Decision

Use the **NXP FRDM-RW612** for firmware development now. A smaller devkit (likely
ESP32-C3, per current intent) will be chosen later once the protocol/session logic is
proven out and a final form factor matters.

## Consequences

- MVP1 firmware work (USB host bring-up, HID framing, session handshake, KeepAlive, preset
  fetch) targets `frdm_rw612` and NXP's MCUX-based Zephyr port, not Espressif's.
- BLE peripheral work for MVP2 uses RW612's onboard BLE 5.3 radio (binary blobs fetched via
  `west blobs fetch hal_nxp`, per the board's Zephyr docs).
- **The eventual switch to a smaller board (e.g. ESP32-C3) will require rewriting the USB
  host transport layer regardless of this choice** — RW612's EHCI driver and the ESP32-S3's
  DWC2 driver are equally non-portable to a chip like the C3, which has no native USB-OTG
  silicon at all. That migration will most likely mean adopting an external MAX3421E-based
  USB host controller (Zephyr's one non-experimental reference UHC driver) at that time. This
  isn't a cost created by choosing RW612 over the S3 — it exists either way — so it's
  deliberately not being optimized against now.
- Protocol/session logic written above the raw transport (HID reassembly, protobuf decode,
  session handshake, KeepAlive) stays portable across that future switch; only the
  lowest-level transport driver and its Kconfig/devicetree wiring will need replacing.
- If NXP's host-side driver maturity stalls or a blocking gap turns up during MVP1 bring-up
  (#3), that's grounds to revisit this ADR — not to silently work around it.
