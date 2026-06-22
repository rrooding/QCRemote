# Phases 2–7 — Implementation Guide

## Overview

After Phase 1 (Xcode setup), you'll implement the remaining 6 phases in order. Each phase builds on the previous one and has clear deliverables.

---

## Phase 2 — Data Models

### Objective
Implement the three SwiftData `@Model` classes and supporting enums.

### Files to Create
- `Models/Preset.swift`
- `Models/Scene.swift`
- `Models/SyncMetadata.swift`
- `Models/SyncSource.swift`

### Deliverables

#### Preset.swift
```swift
import Foundation
import SwiftData

@Model
final class Preset {
    var id: UUID
    var name: String
    var bankIndex: Int  // 0-based
    var slotIndex: Int  // 0–3
    var midiProgramNumber: Int  // 0–127
    @Relationship(deleteRule: .cascade, inverse: \Scene.preset)
    var scenes: [Scene]
    var lastSyncDate: Date
    var syncSource: SyncSource
    
    init(
        id: UUID = UUID(),
        name: String,
        bankIndex: Int,
        slotIndex: Int,
        midiProgramNumber: Int,
        scenes: [Scene] = [],
        lastSyncDate: Date = Date(),
        syncSource: SyncSource
    ) {
        self.id = id
        self.name = name
        self.bankIndex = bankIndex
        self.slotIndex = slotIndex
        self.midiProgramNumber = midiProgramNumber
        self.scenes = scenes
        self.lastSyncDate = lastSyncDate
        self.syncSource = syncSource
    }
    
    var bankLabel: String {
        "Bank \(bankIndex + 1)"
    }
    
    var slotLabel: String {
        ["A", "B", "C", "D"][slotIndex]
    }
    
    var bankAndSlot: String {
        "\(bankLabel) / \(slotLabel)"
    }
}
```

#### Scene.swift
```swift
import Foundation
import SwiftData

@Model
final class Scene {
    var id: UUID
    var name: String
    var index: Int  // 0–7
    var preset: Preset?  // back-reference
    
    init(
        id: UUID = UUID(),
        name: String,
        index: Int,
        preset: Preset? = nil
    ) {
        self.id = id
        self.name = name
        self.index = index
        self.preset = preset
    }
}
```

#### SyncSource.swift
```swift
import Foundation

enum SyncSource: String, Codable {
    case backupFile
    case cortexCloud
    case usbDirect
    
    var displayName: String {
        switch self {
        case .backupFile: "Backup File"
        case .cortexCloud: "Cortex Cloud"
        case .usbDirect: "USB Direct"
        }
    }
}
```

#### SyncMetadata.swift
```swift
import Foundation
import SwiftData

@Model
final class SyncMetadata {
    var source: SyncSource
    var lastSyncDate: Date
    var displayName: String
    
    init(
        source: SyncSource,
        lastSyncDate: Date = Date(),
        displayName: String
    ) {
        self.source = source
        self.lastSyncDate = lastSyncDate
        self.displayName = displayName
    }
}
```

### Update QCRemoteApp.swift

Modify the `ModelContainer` initialization to include the models:

```swift
let schema = Schema([
    Preset.self,
    Scene.self,
    SyncMetadata.self,
])
```

### Testing

1. Build the project (Cmd+B) — should compile
2. Create a test to verify the models work:

```swift
// In some test file or a simple ViewController
let preset = Preset(
    name: "Test",
    bankIndex: 0,
    slotIndex: 0,
    midiProgramNumber: 0,
    syncSource: .backupFile
)
print(preset.bankAndSlot)  // "Bank 1 / A"
```

---

## Phase 3 — MIDI Engine

### Objective
Implement the `MIDIEngine` actor, `MIDIConfiguration`, and `MIDIEvent` types.

### Files to Create
- `MIDI/MIDIConfiguration.swift`
- `MIDI/MIDIEvent.swift`
- `MIDI/MIDIError.swift`
- `MIDI/MIDIEngine.swift`

### Key Decisions Before Implementing

1. **MIDI setup approach:** CoreMIDI callbacks are complex. You can either:
   - Use low-level CoreMIDI APIs directly (more control, more code)
   - Use a third-party library like `SwiftMIDI` (simpler, but adds dependency)

   Recommendation: Start with low-level CoreMIDI using `CoreMIDI` framework (Apple's official).

2. **AsyncStream vs Combine:** You could use Combine publishers or SwiftUI 5.10+'s `AsyncStream`. The design specifies `AsyncStream` (more modern).

3. **MIDI packet handling:** Incoming MIDI is delivered as raw bytes. You'll need to:
   - Decode Status byte to determine message type (0x90 = Note On, 0xB0 = CC, 0xC0 = PC, etc.)
   - Decode channel from status byte
   - Decode data bytes (value, controller, etc.)

### Implementation Notes

- **Callbacks on background thread:** CoreMIDI callbacks arrive on a system thread, not main. The actor enforces this isolation.
- **Stream creation:** `AsyncStream` needs a closure to yield values. This maps to your MIDI event callback.
- **Outgoing MIDI:** Sending PC/CC is simpler — just pack bytes and send via output port.

### Testing

Once implemented:
1. Download **MIDI Monitor.app** (free on App Store) or use **Audio MIDI Setup.app**
2. Create a virtual MIDI port
3. Route your app's MIDI to that port
4. Verify MIDI events are decoded correctly
5. Verify outgoing PC/CC messages appear in the monitor

---

## Phase 4 — PresetLibrary & Providers

### Objective
Implement the `PresetLibrary` class and three provider classes.

### Files to Create
- `Library/PresetLibrary.swift`
- `Providers/PresetLibraryProvider.swift`
- `Providers/PresetLibraryError.swift`
- `Providers/BackupFileProvider.swift`
- `Providers/CortexCloudProvider.swift`
- `Providers/USBDirectProvider.swift`

### Key Challenges

#### BackupFileProvider: Understanding `.qcb` Format

**Before you implement this, you need:**
1. A real `.qcb` backup file from your Quad Cortex (export from the QC's menu)
2. Unzip it and examine the JSON structure

Steps:
1. On your QC: Menu → Settings → System → Backup → Export
2. Transfer the `.qcb` file to your Mac
3. In Terminal: `unzip -l my-backup.qcb` to list contents
4. Examine JSON files (likely `presets.json`, `manifest.json`, etc.)
5. Document the schema (field names, structure) in `docs/OPEN-QUESTIONS.md`

**Then implement:**
- Read `.qcb` as a ZIP archive (use `Foundation.Zip`)
- Parse JSON to extract preset names, scene names, bank info
- Map to your `Preset` and `Scene` objects

#### CortexCloudProvider: API Reverse-Engineering

**This is undocumented, so you'll need to:**
1. Log into cloud.neuraldsp.com in a browser
2. Open Developer Tools (F12) → Network tab
3. Interact with the web app (load presets, etc.)
4. Capture the API requests and responses
5. Document the endpoints, auth method, response format

**Common patterns:**
- Auth endpoint: usually `/api/auth/login` or `/api/signin`
- Presets endpoint: `/api/presets` or `/api/user/presets`
- Auth: typically Bearer token in `Authorization: Bearer <token>` header

**Store API findings in `docs/OPEN-QUESTIONS.md` with curl examples.**

### PresetLibrary Lookup Function

The critical function for Phase 5:
```swift
@MainActor
class PresetLibrary {
    let modelContext: ModelContext
    
    func preset(for programNumber: Int) -> Preset? {
        var descriptor = FetchDescriptor<Preset>(
            predicate: #Predicate { $0.midiProgramNumber == programNumber }
        )
        descriptor.fetchLimit = 1
        return try? modelContext.fetch(descriptor).first
    }
}
```

This is called on *every MIDI event* (potentially many times per second), so it must be fast. SwiftData queries are fine for this.

---

## Phase 5 — AppState

### Objective
Wire together MIDI engine and preset library into observable app state.

### Files to Create
- `State/AppState.swift`
- `State/AppStateError.swift`

### Key Implementation Points

```swift
@MainActor @Observable
class AppState {
    // Observable properties
    var currentPreset: Preset?
    var currentSceneIndex: Int = 0
    var isLastKnownState: Bool = true
    var midiConnectionStatus: MIDIConnectionStatus = .disconnected
    
    let midiEngine: MIDIEngine
    let presetLibrary: PresetLibrary
    
    init(midiEngine: MIDIEngine, presetLibrary: PresetLibrary) {
        self.midiEngine = midiEngine
        self.presetLibrary = presetLibrary
    }
    
    func start() async {
        // Step 1: Restore last known state
        if let (programNumber, sceneIndex) = UserDefaults.standard.lastKnownState {
            self.currentPreset = presetLibrary.preset(for: programNumber)
            self.currentSceneIndex = sceneIndex
            self.isLastKnownState = true
        }
        
        // Step 2: Begin listening to MIDI events
        Task {
            for await event in await midiEngine.events {
                self.onMIDIEvent(event)
            }
        }
    }
    
    private func onMIDIEvent(_ event: MIDIEvent) {
        switch event {
        case .presetChanged(let pc, _, _):
            if let preset = presetLibrary.preset(for: pc) {
                self.currentPreset = preset
                self.persistLastKnownState()
                self.isLastKnownState = false
            }
        case .sceneChanged(let index):
            self.currentSceneIndex = index
        case .connectionChanged(let status):
            self.midiConnectionStatus = status
        }
    }
    
    func selectScene(_ index: Int) async {
        do {
            try await midiEngine.selectScene(index)
        } catch {
            print("Failed to select scene: \(error)")
        }
    }
    
    func selectPreset(_ preset: Preset) async {
        do {
            try await midiEngine.selectPreset(preset.midiProgramNumber)
        } catch {
            print("Failed to select preset: \(error)")
        }
    }
    
    private func persistLastKnownState() {
        guard let preset = currentPreset else { return }
        UserDefaults.standard.setLastKnownState(
            (preset.midiProgramNumber, currentSceneIndex)
        )
    }
}

// UserDefaults extension
extension UserDefaults {
    var lastKnownState: (Int, Int)? {
        guard let data = data(forKey: "lastKnownState") else { return nil }
        let decoder = JSONDecoder()
        return try? decoder.decode((Int, Int).self, from: data)
    }
    
    func setLastKnownState(_ value: (Int, Int)) {
        let encoder = JSONEncoder()
        if let data = try? encoder.encode(value) {
            set(data, forKey: "lastKnownState")
        }
    }
}
```

---

## Phase 6 — UI Views

### Objective
Build all SwiftUI views and integrate AppState.

### Files to Create
- `UI/ContentView.swift` (or `RootView.swift`)
- `UI/PresetHeaderView.swift`
- `UI/SceneGridView.swift`
- `UI/PresetNavigationView.swift`
- `UI/SettingsSheet.swift`

### Layout Strategy

Use `@Environment(\.horizontalSizeClass)` to adapt layouts:
- **iPhone (compact):** Single-column layout, 2×4 scene grid, bottom nav
- **iPad/Mac (regular):** Wider layout, 4×2 scene grid, preset name larger

See `docs/ARCHITECTURE.md` for detailed view code examples.

### Update App Entry Point

```swift
@main
struct QCRemoteApp: App {
    let modelContainer: ModelContainer
    @State private var appState: AppState
    @State private var presetLibrary: PresetLibrary
    @State private var midiEngine: MIDIEngine
    
    var body: some Scene {
        WindowGroup {
            ContentView()
                .environment(appState)
                .environment(presetLibrary)
                .environment(midiEngine)
                .task {
                    await appState.start()
                }
        }
        .modelContainer(modelContainer)
    }
}
```

---

## Phase 7 — Polish & Edge Cases

### Things to Handle

1. **Unknown preset:** Program number arrives that doesn't exist in cache
   - Show "Unknown Preset #42" instead of crashing
   - Don't update `currentPreset` to nil

2. **MIDI connection lost/regained:** Handle gracefully
   - Show connection status in UI
   - Queue MIDI commands if disconnected, retry when reconnected (or just fail and let user retry)

3. **Import errors:**
   - Malformed `.qcb` file
   - Invalid JSON
   - Missing required fields
   - Show error alert to user with details

4. **Cloud auth failures:**
   - Wrong credentials
   - Network error
   - Token expiry during use
   - Retry mechanism

5. **Scene changes before preset loads:**
   - User changes scene, but current preset is still nil/loading
   - Queue the scene change or show a warning

### Testing Checklist

- [ ] Import a real `.qcb` file, all presets appear
- [ ] Sign in to Cortex Cloud, sync presets
- [ ] Connect WIDI Jack, change preset on QC, app updates
- [ ] Tap a scene in app, QC switches
- [ ] Kill app and relaunch, "Last known" badge appears
- [ ] Change preset on QC, badge clears
- [ ] Tap next/prev preset buttons, MIDI sends
- [ ] MIDI config (channel, CC) can be changed and persists
- [ ] On iPad/Mac, layout is wider and more readable
- [ ] Handles unknown preset numbers gracefully
- [ ] Shows MIDI connection status

---

## Tips for Implementation

1. **Test incrementally:** After each phase, run the app and verify the component works. Don't wait until the end.
2. **Use print statements liberally:** CoreMIDI and SwiftData can be opaque. Log what's happening.
3. **MIDI on simulator:** You can't test real MIDI on the iOS simulator easily. Either:
   - Test on a physical device connected to WIDI Jack
   - Create a virtual MIDI port and route your app to it (more setup)
   - Skip MIDI testing until you have a device
4. **SwiftData queries:** Keep fetch descriptors simple for performance. The `preset(for:)` lookup is called potentially hundreds of times.
5. **Async/await:** Be careful with Task creation. The `start()` method spawns a Task to consume the MIDI stream — this should live for the app's lifetime.

---

## References

- [SwiftData docs](https://developer.apple.com/documentation/swiftdata/)
- [CoreMIDI docs](https://developer.apple.com/documentation/coremidi)
- [AsyncStream docs](https://developer.apple.com/documentation/swift/asyncstream)
- [SwiftUI @Observable](https://developer.apple.com/documentation/observation/observable)

---

## Debugging Tips

| Problem | Debugging approach |
|---|---|
| MIDI not being received | Check MIDI Monitor.app, verify QC is sending MIDI |
| Preset not updating on UI | Add print statements in `onMIDIEvent` to confirm event is received |
| SwiftData queries returning empty | Check ModelContainer is initialized, preset data actually saved |
| Cloud login failing | Capture network traffic with Charles Proxy or browser DevTools, check auth endpoint |
| "Last known" badge not clearing | Verify MIDI event arrives and `isLastKnownState` is set to false |
