# QC Remote

A wireless companion screen app for the Neural DSP Quad Cortex guitar processor. Connects via WIDI Jack (BLE MIDI), displays current preset/scene names, and allows scene/preset switching from the app.

**Status:** Design phase complete, ready for Phase 1 implementation (Xcode project setup).

**Platforms:** iOS 17+, iPadOS 17+, macOS 14+
**Language:** Swift / SwiftUI
**Data:** SwiftData for local caching

## Quick Start (on macBook)

1. Read `docs/DESIGN.md` for the full design overview
2. Read `docs/ARCHITECTURE.md` for technical details
3. Follow `docs/PHASE-1-SETUP.md` to create the Xcode project
4. See `docs/OPEN-QUESTIONS.md` for research items before Phase 2

## Structure

```
QCRemote/
├── docs/
│   ├── DESIGN.md              (full design & rationale)
│   ├── ARCHITECTURE.md        (technical architecture deep-dive)
│   ├── PHASE-1-SETUP.md       (step-by-step Xcode project creation)
│   ├── PHASE-2-onwards.md     (phases 2-7)
│   └── OPEN-QUESTIONS.md      (research items)
├── Models/                    (SwiftData models: Preset, Scene, SyncMetadata)
├── MIDI/                      (MIDIEngine, MIDIConfiguration, MIDIEvent)
├── Library/                   (PresetLibrary)
├── Providers/                 (PresetLibraryProvider protocol + implementations)
├── State/                     (AppState)
├── UI/                        (SwiftUI views)
└── Tests/                     (unit tests)
```

## Design Summary

- **Data flow:** CoreMIDI → MIDIEngine (actor) → AppState (@MainActor) → PresetLibrary (@MainActor) → SwiftData (read-only)
- **Providers:** BackupFileProvider (.qcb), CortexCloudProvider (cloud API), USBDirectProvider (stub)
- **State:** Last known preset/scene cached in UserDefaults, shown with "last known" badge until MIDI confirms
- **UI:** Responsive layout (iPhone 2×4 scene grid, iPad/Mac 4×2), dark background for stage visibility

## Approved Plan

See `/home/ralph/.claude/plans/i-have-a-quad-velvety-rossum.md` for the complete approved design plan.
