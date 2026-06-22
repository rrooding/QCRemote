# QC Second Screen — Design & Implementation Plan (APPROVED)

This is the approved design plan created during the brainstorming and planning phase. See `DESIGN.md` and `ARCHITECTURE.md` for expanded explanations.

---

## Context

A wireless "second screen" companion app for the Neural DSP Quad Cortex guitar processor. The QC connects wirelessly via a WIDI Jack BLE MIDI adapter. The app displays the current preset and scene names, lets the user tap to change scenes/presets, and sends MIDI commands back to the QC.

Because MIDI only tells you *which* preset/scene is active (not what it's called), the app loads preset/scene metadata from a separate data source (backup file or Cortex Cloud) and caches it locally. The app is fully functional with only the WIDI connection as long as the local cache has been populated at least once.

Target: SwiftUI native app, iOS 17+ / macOS 14+. One device at a time connects to the WIDI Jack. Supported targets: iPhone, iPad, Mac.

---

## Architecture

```
CoreMIDI callbacks
      │
      ▼
MIDIEngine (Actor)          ← background thread, processes raw MIDI
      │  AsyncStream<MIDIEvent>
      ▼
AppState (@MainActor @Observable)   ← consumes stream, drives UI
      │  preset(for programNumber:)
      ▼
PresetLibrary (@MainActor @Observable)
      │  reads from
      ▼
SwiftData (ModelContainer)  ← always the single read source for UI
      ▲
      │  writes via
PresetLibraryProvider (protocol)
      ├── BackupFileProvider     (.qcb import)
      ├── CortexCloudProvider    (Neural DSP API, Keychain auth)
      └── USBDirectProvider      (stub — throws .notImplemented)
```

---

## Data Models (SwiftData)

```swift
@Model class Preset {
    var id: UUID
    var name: String
    var bankIndex: Int          // 0-based
    var slotIndex: Int          // 0–3 → A/B/C/D
    var midiProgramNumber: Int  // 0–127 (PC value QC sends)
    var scenes: [Scene]
    var lastSyncDate: Date
    var syncSource: SyncSource  // .backupFile | .cortexCloud | .usbDirect
}

@Model class Scene {
    var id: UUID
    var name: String
    var index: Int              // 0–7
    var preset: Preset
}

@Model class SyncMetadata {
    var source: SyncSource
    var lastSyncDate: Date
    var displayName: String     // e.g. "backup-2025-06-22.qcb"
}

enum SyncSource: String, Codable { case backupFile, cortexCloud, usbDirect }
```

**Key lookup:** `PresetLibrary.preset(for programNumber: Int) -> Preset?` — called on every incoming MIDI PC event.

Bank select (CC0/CC32 + PC) for >128 presets: handled by extending `midiProgramNumber` to a `(bankMSB, bankLSB, programNumber)` tuple when needed. Start with program-only and extend if the user has >128 presets.

---

## MIDI Engine

```swift
actor MIDIEngine {
    // CoreMIDI client, input port, output port
    // Reads config from MIDIConfiguration

    func start() async               // sets up CoreMIDI, begins listening
    var events: AsyncStream<MIDIEvent> { get }

    func selectPreset(_ programNumber: Int) async  // sends PC out
    func selectScene(_ index: Int) async           // sends CC34 (configurable) out
}

struct MIDIConfiguration: Codable {  // persisted in UserDefaults
    var receiveChannel: Int = 1      // 1–16
    var sceneChangeCC: Int = 34      // QC default
    var allChannels: Bool = false
}

enum MIDIEvent {
    case presetChanged(programNumber: Int, bankMSB: Int, bankLSB: Int)
    case sceneChanged(index: Int)
    case connectionChanged(MIDIConnectionStatus)
}

enum MIDIConnectionStatus { case connected, disconnected }
```

CoreMIDI delivers callbacks on a background thread — the actor boundary enforces this isolation. `AppState` consumes `events` in a `Task` on `@MainActor`.

---

## PresetLibrary & Providers

```swift
protocol PresetLibraryProvider {
    var id: SyncSource { get }
    var displayName: String { get }
    func fetchLibrary() async throws -> [Preset]
}

// BackupFileProvider
// - Accepts URL to .qcb file (ZIP archive containing JSON)
// - Unzips with Foundation, parses JSON into [Preset]
// - Stateless: call fetchLibrary() on each import

// CortexCloudProvider
// - Stores credentials in Keychain (via Security framework)
// - func signIn(email: String, password: String) async throws
// - Calls Neural DSP API (undocumented — mark with FRAGILE comment)
// - fetchLibrary() returns cloud presets

// USBDirectProvider
// - Conforms to PresetLibraryProvider
// - fetchLibrary() throws PresetLibraryError.notImplemented
// - Slot exists so the architecture is complete

@MainActor @Observable
class PresetLibrary {
    let modelContext: ModelContext
    var syncMetadata: [SyncMetadata]

    func sync(from provider: PresetLibraryProvider) async throws
    // → fetchLibrary(), upserts into SwiftData, updates SyncMetadata

    func preset(for programNumber: Int) -> Preset?
}
```

Providers are called only on explicit user action (Import / Sync button). No background polling.

---

## AppState

```swift
@MainActor @Observable
class AppState {
    var currentPreset: Preset?       // nil until first MIDI event
    var currentSceneIndex: Int = 0
    var isLastKnownState: Bool = true  // cleared by first MIDI event
    var midiConnectionStatus: MIDIConnectionStatus = .disconnected

    // Injected dependencies
    let midiEngine: MIDIEngine
    let presetLibrary: PresetLibrary

    func start() async {
        // Restore last known preset from UserDefaults
        // Begin consuming midiEngine.events stream
    }

    func onMIDIEvent(_ event: MIDIEvent) {
        // presetChanged → lookup in presetLibrary, set currentPreset,
        //                  persist programNumber to UserDefaults,
        //                  clear isLastKnownState
        // sceneChanged  → update currentSceneIndex
        // connectionChanged → update midiConnectionStatus
    }

    func selectScene(_ index: Int) async   // calls midiEngine.selectScene
    func selectPreset(_ preset: Preset) async  // calls midiEngine.selectPreset
}
```

Last known state persisted to `UserDefaults` as `(programNumber, sceneIndex)`. On next launch, `AppState` restores this and shows it with the "last known" badge until MIDI confirms the current state.

---

## UI Structure

```
RootView (adaptive: iPhone / iPad / Mac)
  ├── PresetHeaderView
  │     • Bank label "3 / C"  +  Preset name (large, high contrast)
  │     • "Last known" badge — visible until first MIDI event
  │     • MIDI connection indicator dot (green / red)
  │
  ├── SceneGridView
  │     • 8 tappable buttons  (2×4 on iPhone, 4×2 on iPad/Mac)
  │     • Active scene highlighted
  │     • Names from SwiftData; falls back to "Scene 1–8" if unnamed
  │
  ├── PresetNavigationView
  │     • Previous / Next buttons → send MIDI PC out
  │
  └── SettingsSheet
        • MIDI Configuration: channel (1–16), scene CC number (default 34)
        • Data Sources:
            – Import Backup File  (file picker → BackupFileProvider)
            – Sign in to Cortex Cloud  (CortexCloudProvider)
        • Last sync info per source
```

**Responsive layout:** iPhone uses single-column with bottom navigation bar. iPad/Mac uses a wider layout with preset name prominent — readable across a room. All tap targets ≥44pt. Dark background default for stage readability.

---

## Implementation Phases

### Phase 1 — Project setup
- New Xcode project: SwiftUI, targets iOS 17+ and macOS 14+ (Mac Catalyst or native macOS target)
- Folder structure: `Models/`, `MIDI/`, `Library/`, `Providers/`, `State/`, `UI/`
- SwiftData `ModelContainer` set up in `App` entry point

### Phase 2 — Data models
- Implement `Preset`, `Scene`, `SyncMetadata` SwiftData models
- Write unit tests for `preset(for programNumber:)` lookup

### Phase 3 — MIDI Engine
- Implement `MIDIEngine` actor with CoreMIDI
- `MIDIConfiguration` UserDefaults persistence
- `AsyncStream<MIDIEvent>` output
- Send PC and CC commands out
- Test with MIDI monitor tool (e.g. MIDI Monitor.app)

### Phase 4 — PresetLibrary & Providers
- `PresetLibraryProvider` protocol
- `BackupFileProvider`: parse `.qcb` (ZIP → JSON → SwiftData upsert)
  - Research `.qcb` JSON schema from community docs / sample files
- `CortexCloudProvider`: reverse-engineer Neural DSP API from browser network traffic
  - Mark all API calls with `// FRAGILE: undocumented API`
  - Keychain storage for credentials
- `USBDirectProvider`: stub only
- `PresetLibrary` sync and lookup methods

### Phase 5 — AppState
- Wire `MIDIEngine` events to `AppState`
- Last known state persistence (UserDefaults)
- "Last known" badge logic

### Phase 6 — UI
- `PresetHeaderView`, `SceneGridView`, `PresetNavigationView`
- `SettingsSheet` with MIDI config and data source management
- Adaptive layout (iPhone / iPad / Mac)
- Dark, high-contrast styling

### Phase 7 — Polish & edge cases
- "Unknown preset" state (program number not in cache)
- MIDI connection lost / reconnected handling
- Import error handling (malformed .qcb, wrong file)
- Cloud auth failure / token expiry

---

## Open Questions to Resolve During Implementation

- **`.qcb` JSON schema:** Need a sample backup file to confirm field names for preset name, scene names, bank/slot positions. Community resources: [qcbackup community thread / Neural DSP forums].
- **Cortex Cloud API:** Requires capturing network traffic from a browser session logged into cloud.neuraldsp.com. Endpoints and auth token format TBD.
- **Bank select:** Confirm whether the user has >128 presets; if not, bank select can be deferred.

---

## Verification

1. **MIDI Engine:** Use MIDI Monitor.app on Mac to verify the app sends correct PC/CC messages when scenes/presets are tapped. Verify incoming PC/CC events update the UI correctly.
2. **BackupFileProvider:** Import a real `.qcb` file, confirm all preset and scene names appear in the app.
3. **CortexCloudProvider:** Sign in, sync, confirm presets match what's visible in Cortex Control.
4. **End-to-end:** Connect WIDI Jack, change preset on QC hardware, confirm app updates. Tap a scene in the app, confirm QC switches to that scene.
5. **Startup state:** Kill and relaunch app, confirm last known preset shows with badge. Change preset on QC, confirm badge clears.
6. **Platform targets:** Run on iPhone simulator, iPad simulator, and Mac — confirm layout adapts correctly.

---

**Plan Status:** ✅ Approved and ready for implementation
