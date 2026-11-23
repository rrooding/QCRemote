# QCRemote

A remote control for a Neural DSP Quad Cortex: a microcontroller board talks to the Quad Cortex over USB and serves a web page that controls it over Wi-Fi.

## Hardware

**Board**:
The microcontroller running QCRemote firmware, sitting between the Quad Cortex and the web page.
_Avoid_: devboard, device, bridge

**Quad Cortex** (**QC**):
The Neural DSP amp modeller being controlled. Spelled out or abbreviated as QC.
_Avoid_: device, unit, amp

## Health

**Link**:
The state of the Board's USB connection to the Quad Cortex.
_Avoid_: connection (that's the page's side), USB state

**Connection**:
The state of the web page's WebSocket connection to the Board.
_Avoid_: link (that's the USB side), socket

## Presets

**Catalog**:
The Board's full listing of the Quad Cortex's folders and presets, as of one generation.
_Avoid_: preset list, library

**Generation**:
A number identifying one version of the Catalog; it changes whenever the Board re-reads the listing.
_Avoid_: revision, version

**Folder**:
A named group of Presets in the Catalog.
_Avoid_: bank, setlist, directory

**Preset**:
One entry in the Catalog, identified by its index across the whole Catalog (not within its Folder).
_Avoid_: patch, slot, program

**Current preset**:
The Preset the Quad Cortex reports as loaded.
_Avoid_: active preset, selected preset
