# ADR 0009: Support the ESP32-S3 as a second firmware target

Date: 2026-11-04
Status: Proposed

## Context

The RevA board needs an MCU with a USB host, Wi-Fi and Bluetooth LE that JLCPCB can assemble
cheaply. The firmware so far runs on the NXP RW612 ([ADR 0002](0002-devkit-selection.md)),
which has a High Speed (480 Mbit/s) USB host. The candidates looked at for RevA:

- **u-blox IRIS-W1 (RW612 module)**: High Speed, dual-band Wi-Fi 6, BLE, and the firmware
  runs unchanged. About $27 per module at JLCPCB, pre-order only.
- **ESP32-S3-WROOM-1-N16R8**: Full Speed (12 Mbit/s) USB host only, 2.4 GHz Wi-Fi, BLE 5,
  8 MB PSRAM. About $5.40, tens of thousands in stock at JLCPCB.
- **ESP32-S31**: High Speed USB host, but no Zephyr support yet, and only on pre-order.
  Its WROOM-1 isn't pin-compatible with the S3's.
- **STM32 + Wi-Fi companion**: no Zephyr USB host driver for STM32's OTG controller, and no
  cheaper than the IRIS-W1.
- **Nordic nRF7120**: High Speed USB, Wi-Fi 6 and BLE on one chip, but only sampling.

The S3 depends on whether the Quad Cortex works over Full Speed. The data rate needed is low:
the web page recreates the device screen once per change, with no live streaming. But the
Quad Cortex reports `wMaxPacketSize 128` on its interrupt endpoint even at Full Speed, above
USB 2.0's 64-byte limit, and bursts ~90 KB of replies right after connecting.

Measured on a Quad Cortex Mini (CorOS 4.1.1), issue #76:

- An RW612 forced to Full Speed (`CONFIG_QC_USBH_FORCE_FULL_SPEED`) ran the session, but lost
  about half the reports when the File READ overlapped the state READs' reply burst, and the
  listing came back empty. Holding the File READ until that burst drained fixed it.
- An ESP32-S3-DevKitC-1 accepted the 128-byte endpoint. Once three firmware issues were fixed
  and Zephyr's DWC2 driver was patched (see Consequences), it gave a File stream identical to
  the RW612's at High Speed: 21 messages, 29599 bytes, 0 malformed, 3 folders / 48 presets.

## Decision

Build the firmware for both `frdm_rw612` and `esp32s3_devkitc/esp32s3/procpu` from one tree,
and treat the ESP32-S3 as the RevA MCU unless later firmware work (Wi-Fi together with USB, or
the screen recreation's data rate) shows it can't keep up. Nothing here removes the RW612
path; choosing the IRIS-W1 instead stays open until the RevA layout is committed.

## Consequences

- RevA costs about $22 less per board in the MCU module, and is easy to source at JLCPCB.
- Full Speed leaves little room for bursts: one 128-byte packet per 1 ms frame, two per
  report, so about 500 reports/s, and the device drops what it can't send without any error
  we can detect. The firmware has to keep its own requests spaced out (the File READ waits
  for the state READ burst to settle), and later features that move more data must respect
  that ceiling.
- The firmware now joins reports split across transfers, needed because the DWC2 driver
  completes interrupt IN per packet. This is harmless on the RW612, which delivers whole reports.
- The S3 needs a local Zephyr patch (`firmware/patches/zephyr/`) until it's fixed upstream:
  without it, interrupt IN stalls for ~16.4 s after connect. Every workspace that builds for
  the S3 has to apply it, and it should be sent upstream.
- The S3's internal RAM can't hold the two 128 KiB message buffers from ADR 0007. The devkit
  build uses 64 KiB (`CONFIG_QC_MESSAGE_CAPACITY`). RevA's N16R8 module has 8 MB PSRAM, so
  those buffers should move there for the RevA board config.
- Zephyr's USB host stack and its DWC2 host driver are still experimental, and interrupt
  transfer support only landed in September 2026. Expect more driver issues than on the RW612.
- Moving to the ESP32-S31 later, if it gets Zephyr support, needs a new board revision: same
  outline, but its High Speed USB is on pads underneath the module.
