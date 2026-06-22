# QC Remote — Full Design

## What is QC Remote?

A wireless companion screen for your Neural DSP Quad Cortex guitar processor. The QC connects to your app via a WIDI Jack (BLE MIDI). The app shows you the current preset name, scene name, and lets you tap to switch scenes/presets — all wirelessly, without needing to look at the QC hardware itself.

**Use case:** On stage, you have your QC and your wireless companion app (on iPhone, iPad, or Mac). Change presets on the app, QC updates. Or change on the QC hardware, app updates. You always know what preset/scene you're on.

## Why Build It This Way?

The QC sends MIDI when you change presets/scenes, but MIDI only tells you *which* preset you're on (a number 0–127), not what it's *called*. So the app loads your preset library from one of two sources:

1. **Backup file** (`.qcb` — the file QC exports): fully offline, stable, but manual re-import when you change preset names
2. **Cortex Cloud** (login to Neural DSP cloud): always fresh, but requires reverse-engineering an undocumented API

The app caches everything locally in SwiftData. Once you've imported your preset library, the app is fully functional with just the WIDI BLE connection — no internet needed, no repeated cloud calls.

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│  CoreMIDI (system framework)                    │
│  Receives BLE MIDI events from WIDI Jack        │
└─────────────────┬───────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────┐
│  MIDIEngine (Swift Actor)                       │
│  • Manages CoreMIDI session                     │
│  • Receives MIDI callbacks (background thread) │
│  • Emits typed MIDIEvent via AsyncStream       │
│  • Sends PC/CC messages out to QC              │
└─────────────────┬───────────────────────────────┘
                  │ AsyncStream<MIDIEvent>
                  ▼
┌─────────────────────────────────────────────────┐
│  AppState (@MainActor @Observable)             │
│  • Consumes MIDIEngine events stream            │
│  • Looks up Preset via PresetLibrary            │
│  • Updates @Observable properties for UI       │
│  • Persists last known state to UserDefaults   │
└─────────────────┬───────────────────────────────┘
                  │ currentPreset, currentSceneIndex
                  ▼
┌─────────────────────────────────────────────────┐
│  SwiftUI Views                                  │
│  • Display current preset/scene                 │
│  • Tap to change scene (sends MIDI via Engine) │
│  • Settings sheet for sync & config            │
└─────────────────────────────────────────────────┘

Data flow for lookups:
AppState → PresetLibrary.preset(programNumber:) → SwiftData queries
```

**Why actors?** CoreMIDI callbacks arrive on a background thread. Using a Swift `Actor` enforces thread safety at compile time — we can't accidentally access shared state from the wrong thread.

**Why SwiftData as read-only?** Only the providers write to SwiftData (during import/sync). The app always reads from it. This keeps data flow simple: one-way, no bidirectional sync logic.

## Data Models

### Preset (SwiftData @Model)
```swift
@Model class Preset {
    var id: UUID
    var name: String                    // "My Lead Tone"
    var bankIndex: Int                  // 0-based: 0 = Bank 1
    var slotIndex: Int                  // 0–3: A, B, C, D
    var midiProgramNumber: Int          // 0–127, what QC sends as PC
    var scenes: [Scene]                 // up to 8 scenes per preset
    var lastSyncDate: Date              // when this was last imported/synced
    var syncSource: SyncSource          // .backupFile | .cortexCloud | .usbDirect
}

enum SyncSource: String, Codable {
    case backupFile
    case cortexCloud
    case usbDirect  // stub for future USB direct reading
}
```

### Scene (SwiftData @Model)
```swift
@Model class Scene {
    var id: UUID
    var name: String                    // "Verse", "Chorus", etc.
    var index: Int                      // 0–7
    var preset: Preset                  // back-reference
}
```

### SyncMetadata (SwiftData @Model)
```swift
@Model class SyncMetadata {
    var source: SyncSource
    var lastSyncDate: Date
    var displayName: String             // e.g. "backup-2025-06-22.qcb"
}
```

## MIDI Details

The QC sends:
- **Preset change:** Program Change (PC) message, typically 0–127. If you have >128 presets, uses Bank Select (CC0 + CC32) first.
- **Scene change:** CC #34 by default, value 0–7. This is configurable in QC settings.

The app receives both and updates the UI. The app also sends:
- **Preset change:** PC message (+ bank select if needed)
- **Scene change:** CC #34 with value 0–7 (or configured CC number)

Configuration (persisted in UserDefaults):
- `receiveChannel`: MIDI channel 1–16 (default: 1)
- `sceneChangeCC`: CC number for scenes (default: 34)
- `allChannels`: ignore MIDI channel filtering (default: false)

## State Management

### On App Launch
1. `AppState` initializes `MIDIEngine`
2. Engine sets up CoreMIDI, begins listening for MIDI
3. `AppState` restores last known preset from `UserDefaults` (if any)
4. App displays "Last Known: Bank 3 / C — My Lead Tone" with a "last known" badge
5. Badge automatically clears when the first MIDI event arrives confirming the current state

### On MIDI Event
```
MIDIEngine (background thread) 
  → receives CC 34 value 2
  → emits MIDIEvent.sceneChanged(index: 2)
  → AppState (MainActor) consumes event
    → updates currentSceneIndex = 2
    → updates UI (SwiftUI observes @Observable property)
```

### On User Tap (Scene Change)
```
SceneGridView user taps button 4
  → calls AppState.selectScene(4)
  → AppState calls MIDIEngine.selectScene(4)
  → MIDIEngine sends CC 34 value 4 to QC via MIDI
  → (QC switches scene internally)
  → (if enabled, QC sends back CC 34 = 4)
  → AppState receives that MIDI event, updates UI
```

## Provider Architecture

All providers conform to one protocol:

```swift
protocol PresetLibraryProvider {
    var id: SyncSource { get }
    var displayName: String { get }
    func fetchLibrary() async throws -> [Preset]
}
```

### BackupFileProvider
- User picks a `.qcb` file (via file picker)
- Provider unzips it (`.qcb` is a ZIP file)
- Parses JSON files inside (community has documented the schema)
- Returns array of `Preset` objects
- `PresetLibrary.sync(from:)` upserts these into SwiftData

### CortexCloudProvider
- User logs in with Neural DSP account
- Credentials stored in Keychain (iOS/macOS native)
- Provider calls Neural DSP API endpoints (reverse-engineered from browser traffic)
- Returns array of `Preset` objects
- All API calls marked with `// FRAGILE: undocumented API` comments

### USBDirectProvider
- Stub: conforms to protocol, throws `.notImplemented`
- Future enhancement: read state directly from QC via USB (like Cortex Control does)
- Architecture already supports it — just not implemented yet

## UI Layout

### iPhone (single-column, bottom navigation)
```
┌─────────────────────────────────┐
│  Bank 3 / C                     │
│  My Lead Tone                   │
│  ● (green dot: MIDI connected)  │
│  [Last known]                   │
├─────────────────────────────────┤
│  Scene Grid (2×4)               │
│  [1]  [2 active]                │
│  [3]  [4]                       │
│  [5]  [6]                       │
│  [7]  [8]                       │
├─────────────────────────────────┤
│ < Prev   |   Next >             │
│ Presets:  Bank 3 / C            │
├─────────────────────────────────┤
│        [⚙ Settings]             │
└─────────────────────────────────┘
```

### iPad / Mac (wider layout, preset name larger)
```
┌────────────────────────────────────────┐
│ ● Bank 3 / C                           │
│   My Lead Tone (large text)            │
│   [Last known]                         │
├────────────────────────────────────────┤
│ Scene Grid (4×2)                       │
│ [1]  [2]  [3]  [4]                     │
│ [5 active]  [6]  [7]  [8]              │
├────────────────────────────────────────┤
│ Prev Preset     Next Preset            │
│ Bank 3 / C      Bank 3 / D             │
│ My Lead Tone    Rhythm Crunch          │
├────────────────────────────────────────┤
│                    [⚙ Settings]        │
└────────────────────────────────────────┘
```

### SettingsSheet
```
MIDI Configuration
  • Receive Channel: [1] [2] [3] ... [16]
  • Scene Change CC: [34] (text input)

Data Sources
  [Import Backup File]
    Last sync: 2025-06-22 (from "backup.qcb")
  
  [Sign in to Cortex Cloud]
    Last sync: not synced
    Email: [_____________]
    Password: [_____________]
    [Sign In]
```

## Implementation Order (Phases)

1. **Phase 1** — Xcode project setup, folder structure, SwiftData ModelContainer
2. **Phase 2** — Data models (Preset, Scene, SyncMetadata)
3. **Phase 3** — MIDI Engine (CoreMIDI, AsyncStream, thread safety)
4. **Phase 4** — PresetLibrary & Providers (BackupFileProvider first)
5. **Phase 5** — AppState (wire up MIDI events to UI)
6. **Phase 6** — UI (PresetHeaderView, SceneGridView, etc.)
7. **Phase 7** — Polish (error handling, edge cases, cloud provider)

## Key Decisions & Rationale

**Why actor for MIDI, not just MainActor dispatch?**
- CoreMIDI callbacks arrive on a background thread. Using an Actor enforces that MIDI processing happens on one dedicated thread, then publishes to MainActor. No risk of forgetting to dispatch.

**Why SwiftData instead of CoreData?**
- Modern Apple framework, Swift-native, cleaner syntax, fully supported on iOS 17+/macOS 14+.
- Simple data model (no complex relationships), so SwiftData's simplicity is a good fit.

**Why persist last known state in UserDefaults?**
- We need it on every app launch. SwiftData requires a full query; UserDefaults is instant.
- Separate concern: UserDefaults = volatile app state, SwiftData = authoritative preset library.

**Why load from file/cloud explicitly instead of auto-syncing?**
- User controls when to import. No surprise network calls, no hidden API failures.
- Backup file is stable; there's no reason to re-import every launch.
- Cloud: if Neural DSP changes the API, it fails visibly instead of silently.

**Why "last known" badge instead of "unknown"?**
- Better UX on stage: app shows the most relevant recent state, not just "?".
- Badge clearly indicates it's not confirmed yet.
- Clears immediately once MIDI data arrives.

---

## Next Steps

1. **Phase 1 (Xcode setup)** — Follow `docs/PHASE-1-SETUP.md` on your macBook
2. **Research items** — See `docs/OPEN-QUESTIONS.md` for `.qcb` format and Cortex Cloud API endpoints
3. **Phase 2-7** — Detailed steps in `docs/PHASE-2-onwards.md`
