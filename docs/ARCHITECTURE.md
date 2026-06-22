# QC Remote — Technical Architecture

## Module Breakdown

### Models/ — Data Types (SwiftData)

**Files to create:**
- `Preset.swift` — `@Model class Preset { ... }`
- `Scene.swift` — `@Model class Scene { ... }`
- `SyncMetadata.swift` — `@Model class SyncMetadata { ... }`
- `SyncSource.swift` — `enum SyncSource { case backupFile, cortexCloud, usbDirect }`

**Responsibilities:**
- Define the data model
- SwiftData persistence (automatic via @Model)
- Codable conformance where needed

**Note:** These are **read-only from the app's perspective**. Only providers write to them.

---

### MIDI/ — Wireless Communication

**Files to create:**

#### MIDIEngine.swift
```swift
actor MIDIEngine {
    // MARK: - State
    private var midiClient: MIDIClientRef
    private var inputPort: MIDIPortRef
    private var outputPort: MIDIPortRef
    private let configuration: MIDIConfiguration
    
    // MARK: - Initialization
    init(configuration: MIDIConfiguration) async throws
    
    // MARK: - Lifecycle
    func start() async throws  // Begin listening for MIDI events
    func stop() async
    
    // MARK: - Event Stream
    var events: AsyncStream<MIDIEvent> { get }
    // Consumer: for await event in await engine.events { ... }
    
    // MARK: - Output (MIDI -> QC)
    func selectPreset(_ programNumber: Int) async throws
    // Sends Program Change message
    
    func selectScene(_ index: Int) async throws
    // Sends CC #34 (or configured sceneChangeCC) with value
}
```

**Responsibilities:**
- Initialize CoreMIDI session
- Decode incoming MIDI packets to typed events
- Handle MIDI input port callbacks (background thread)
- Send MIDI commands out
- Manage MIDI connection state

**Threading:**
- Actor boundary: all CoreMIDI API calls happen on the actor's executor (not necessarily main thread)
- Callbacks from CoreMIDI are automatically isolated to the actor
- Events are published via `AsyncStream` — safe to consume from any thread

---

#### MIDIConfiguration.swift
```swift
struct MIDIConfiguration: Codable {
    var receiveChannel: Int = 1  // 1–16
    var sceneChangeCC: Int = 34  // QC default
    var allChannels: Bool = false
    
    // Persisted to UserDefaults
    func save()
    static func load() -> MIDIConfiguration
}
```

**Responsibilities:**
- Define MIDI settings
- UserDefaults persistence

---

#### MIDIEvent.swift
```swift
enum MIDIEvent {
    case presetChanged(programNumber: Int, bankMSB: Int, bankLSB: Int)
    case sceneChanged(index: Int)
    case connectionChanged(MIDIConnectionStatus)
}

enum MIDIConnectionStatus {
    case connected
    case disconnected
}

enum MIDIError: Error {
    case initializationFailed
    case inputPortCreationFailed
    case outputPortCreationFailed
    case channelFilteringFailed
}
```

**Responsibilities:**
- Typed event representation
- Error types

---

### Library/ — Preset Lookup & Persistence

**Files to create:**

#### PresetLibrary.swift
```swift
@MainActor @Observable
class PresetLibrary {
    let modelContext: ModelContext  // SwiftData
    
    // MARK: - Sync
    func sync(from provider: PresetLibraryProvider) async throws
    // Calls provider.fetchLibrary()
    // Upserts into SwiftData (merge with existing, update names/scenes)
    // Updates SyncMetadata
    
    // MARK: - Lookup (called on every MIDI event)
    func preset(for programNumber: Int) -> Preset? {
        // Query SwiftData
    }
    
    // MARK: - Inventory (for UI)
    func allPresets(sortedBy: SortOption) -> [Preset]
    func preset(bank: Int, slot: Int) -> Preset?
    
    // MARK: - Metadata
    func lastSyncInfo(for source: SyncSource) -> SyncMetadata?
}

enum SortOption {
    case bankAndSlot
    case name
    case syncDate
}
```

**Responsibilities:**
- Provide lookup API for AppState
- Manage provider coordination
- SwiftData interaction
- Expose metadata about syncs

**Design principle:**
- `PresetLibrary` never goes to the network or reads files — only reads/writes SwiftData
- Providers are called explicitly by AppState or SettingsSheet

---

### Providers/ — Data Sources

**Files to create:**

#### PresetLibraryProvider.swift
```swift
protocol PresetLibraryProvider {
    var id: SyncSource { get }
    var displayName: String { get }
    func fetchLibrary() async throws -> [Preset]
    // Returns the full preset library from this source
    // (Decoding, API calls, etc. happen here)
}

enum PresetLibraryError: Error {
    case invalidFile
    case corruptedData
    case networkError(String)
    case authenticationFailed
    case notImplemented
}
```

**Responsibilities:**
- Protocol definition
- Error types

---

#### BackupFileProvider.swift
```swift
class BackupFileProvider: PresetLibraryProvider {
    let fileURL: URL  // path to .qcb file
    var id: SyncSource { .backupFile }
    var displayName: String { fileURL.lastPathComponent }
    
    init(fileURL: URL)
    
    func fetchLibrary() async throws -> [Preset] {
        // 1. Read .qcb (ZIP file)
        // 2. Extract JSON (e.g., "manifest.json", "presets.json")
        // 3. Parse JSON → Preset objects
        // 4. Return [Preset]
    }
}
```

**Responsibilities:**
- Parse `.qcb` format (ZIP archive)
- Decode JSON to Preset/Scene objects
- Handle file I/O errors gracefully

**Note:** The `.qcb` format is partially documented by the community. You'll need a sample file to confirm the JSON schema before implementing this.

---

#### CortexCloudProvider.swift
```swift
class CortexCloudProvider: PresetLibraryProvider {
    // FRAGILE: Neural DSP does not publicly document this API.
    // Reverse-engineered from browser network traffic.
    // May break at any time if Neural DSP changes endpoints/auth.
    
    var id: SyncSource { .cortexCloud }
    var displayName: String { "Cortex Cloud" }
    
    private var credentials: (email: String, password: String)?
    private var authToken: String?
    
    func signIn(email: String, password: String) async throws
    func signOut()
    
    func fetchLibrary() async throws -> [Preset] {
        // 1. Authenticate (if needed)
        // 2. Call Neural DSP API: GET /api/presets or similar
        // 3. Parse response → Preset objects
        // 4. Return [Preset]
    }
    
    private func authenticate() async throws
    private func makeAPIRequest(_ endpoint: String) async throws -> Data
}
```

**Responsibilities:**
- Authenticate with Neural DSP Cloud
- Call REST API endpoints
- Parse responses
- Handle auth failures (token expiry, wrong credentials)

**Security:**
- Credentials stored in Keychain, never in UserDefaults
- Auth tokens cached in memory only, not persisted

---

#### USBDirectProvider.swift
```swift
class USBDirectProvider: PresetLibraryProvider {
    var id: SyncSource { .usbDirect }
    var displayName: String { "USB Direct (QC connected)" }
    
    func fetchLibrary() async throws -> [Preset] {
        throw PresetLibraryError.notImplemented
    }
}
```

**Responsibilities:**
- Stub: allows architecture to be complete
- Future: implement USB communication (would require reverse-engineering QC's USB protocol)

---

### State/ — Application State

**Files to create:**

#### AppState.swift
```swift
@MainActor @Observable
class AppState {
    // MARK: - Observable Properties
    var currentPreset: Preset?
    var currentSceneIndex: Int = 0
    var isLastKnownState: Bool = true
    var midiConnectionStatus: MIDIConnectionStatus = .disconnected
    
    // MARK: - Dependencies
    let midiEngine: MIDIEngine
    let presetLibrary: PresetLibrary
    
    // MARK: - Initialization
    init(midiEngine: MIDIEngine, presetLibrary: PresetLibrary)
    
    // MARK: - Lifecycle
    func start() async {
        // 1. Restore last known state from UserDefaults
        // 2. Begin consuming midiEngine.events stream
        // 3. For each event: call onMIDIEvent(_:)
    }
    
    // MARK: - Event Handling
    private func onMIDIEvent(_ event: MIDIEvent) {
        switch event {
        case .presetChanged(let pc, _, _):
            if let preset = presetLibrary.preset(for: pc) {
                currentPreset = preset
                persistLastKnownState()
                isLastKnownState = false
            }
        case .sceneChanged(let index):
            currentSceneIndex = index
        case .connectionChanged(let status):
            midiConnectionStatus = status
        }
    }
    
    // MARK: - User Actions
    func selectScene(_ index: Int) async {
        await midiEngine.selectScene(index)
    }
    
    func selectPreset(_ preset: Preset) async {
        await midiEngine.selectPreset(preset.midiProgramNumber)
    }
    
    // MARK: - Persistence
    private func persistLastKnownState()
    private func restoreLastKnownState()
}
```

**Responsibilities:**
- Maintain observable app state
- Consume MIDI events from engine
- Coordinate lookups with PresetLibrary
- Handle user-triggered actions (taps)
- Persist/restore last known state

**Design:**
- `@Observable` so SwiftUI views automatically update
- `@MainActor` ensures all mutations happen on main thread
- Events are consumed in a `Task` spawned during `start()`

---

### UI/ — User Interface

**Files to create:**

#### ContentView.swift (or RootView.swift)
Main view that assembles the layout. Adapts based on `@Environment(\.horizontalSizeClass)`.

```swift
@main
struct QCRemoteApp: App {
    @State private var appState: AppState
    @State private var presetLibrary: PresetLibrary
    @State private var midiEngine: MIDIEngine
    
    var body: some Scene {
        WindowGroup {
            RootView()
                .environment(appState)
                .environment(presetLibrary)
                .environment(midiEngine)
                .task {
                    await appState.start()
                }
        }
    }
}

struct RootView: View {
    @Environment(AppState.self) var appState
    @Environment(\.horizontalSizeClass) var horizontalSize
    
    var body: some View {
        VStack {
            PresetHeaderView()
            SceneGridView()
            PresetNavigationView()
            Spacer()
        }
        .sheet(isPresented: $showSettings) {
            SettingsSheet()
        }
    }
}
```

#### PresetHeaderView.swift
Displays current preset, bank/slot, MIDI status, and "last known" badge.

```swift
struct PresetHeaderView: View {
    @Environment(AppState.self) var appState
    
    var body: some View {
        VStack(alignment: .leading) {
            HStack {
                Circle()
                    .fill(appState.midiConnectionStatus == .connected ? .green : .red)
                    .frame(width: 10, height: 10)
                
                if let preset = appState.currentPreset {
                    VStack(alignment: .leading) {
                        Text("Bank \(preset.bankIndex + 1) / \(slotLabel(preset.slotIndex))")
                            .font(.caption)
                            .foregroundColor(.secondary)
                        Text(preset.name)
                            .font(.title2)
                            .bold()
                    }
                } else {
                    Text("No preset selected")
                        .font(.title2)
                        .foregroundColor(.secondary)
                }
            }
            
            if appState.isLastKnownState {
                Label("Last known", systemImage: "clock.badge.questionmark.fill")
                    .font(.caption)
                    .foregroundColor(.orange)
            }
        }
        .padding()
    }
    
    private func slotLabel(_ index: Int) -> String {
        ["A", "B", "C", "D"][index]
    }
}
```

#### SceneGridView.swift
Grid of 8 scene buttons. Layout adapts: 2×4 on iPhone, 4×2 on iPad/Mac.

```swift
struct SceneGridView: View {
    @Environment(AppState.self) var appState
    @Environment(PresetLibrary.self) var presetLibrary
    @Environment(\.horizontalSizeClass) var horizontalSize
    
    var body: some View {
        let columns = horizontalSize == .compact ? [GridItem(), GridItem()] : [GridItem(), GridItem(), GridItem(), GridItem()]
        
        LazyVGrid(columns: columns, spacing: 12) {
            ForEach(0..<8, id: \.self) { index in
                SceneButton(index: index)
            }
        }
        .padding()
    }
}

struct SceneButton: View {
    let index: Int
    @Environment(AppState.self) var appState
    @Environment(PresetLibrary.self) var presetLibrary
    
    var scene: Scene? {
        appState.currentPreset?.scenes.first(where: { $0.index == index })
    }
    
    var isActive: Bool {
        appState.currentSceneIndex == index
    }
    
    var body: some View {
        Button(action: { Task { await appState.selectScene(index) } }) {
            VStack {
                Text(String(index + 1))
                    .font(.caption)
                Text(scene?.name ?? "Scene \(index + 1)")
                    .font(.caption2)
                    .lineLimit(1)
            }
            .frame(maxWidth: .infinity)
            .frame(height: 60)
            .background(isActive ? Color.blue : Color.gray.opacity(0.3))
            .foregroundColor(.white)
            .cornerRadius(8)
        }
    }
}
```

#### PresetNavigationView.swift
Previous/Next preset buttons for quick navigation.

```swift
struct PresetNavigationView: View {
    @Environment(AppState.self) var appState
    @Environment(PresetLibrary.self) var presetLibrary
    
    var previousPreset: Preset? { ... }
    var nextPreset: Preset? { ... }
    
    var body: some View {
        HStack {
            if let prev = previousPreset {
                Button("< \(prev.bankLabel)") {
                    Task { await appState.selectPreset(prev) }
                }
            }
            
            Spacer()
            
            if let next = nextPreset {
                Button("\(next.bankLabel) >") {
                    Task { await appState.selectPreset(next) }
                }
            }
        }
        .padding()
    }
}
```

#### SettingsSheet.swift
MIDI configuration, data source management (import/sync).

```swift
struct SettingsSheet: View {
    @Environment(AppState.self) var appState
    @Environment(PresetLibrary.self) var presetLibrary
    
    @State private var receiveChannel: Int = 1
    @State private var sceneChangeCC: Int = 34
    @State private var showFileImporter = false
    @State private var cloudEmail = ""
    @State private var cloudPassword = ""
    @State private var isSyncing = false
    
    var body: some View {
        NavigationStack {
            Form {
                Section("MIDI Configuration") {
                    Picker("Receive Channel", selection: $receiveChannel) {
                        ForEach(1...16, id: \.self) { channel in
                            Text("Channel \(channel)")
                        }
                    }
                    
                    Stepper("Scene Change CC: \(sceneChangeCC)", value: $sceneChangeCC, in: 0...127)
                }
                
                Section("Data Sources") {
                    Button("Import Backup File") {
                        showFileImporter = true
                    }
                    
                    VStack(alignment: .leading) {
                        Text("Cortex Cloud")
                            .font(.subheadline)
                        TextField("Email", text: $cloudEmail)
                        SecureField("Password", text: $cloudPassword)
                        
                        Button(isSyncing ? "Syncing..." : "Sign In & Sync") {
                            Task {
                                isSyncing = true
                                // Call cloud provider
                                isSyncing = false
                            }
                        }
                        .disabled(isSyncing || cloudEmail.isEmpty || cloudPassword.isEmpty)
                    }
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
        }
        .fileImporter(
            isPresented: $showFileImporter,
            allowedContentTypes: [.archive],  // .qcb is ZIP
            onCompletion: { result in
                // Load and sync from backup file
            }
        )
    }
}
```

---

## Data Flow Examples

### Example 1: User Changes Scene on App
```
1. User taps scene button 3
2. SceneButton action calls AppState.selectScene(3)
3. AppState.selectScene calls MIDIEngine.selectScene(3)
4. MIDIEngine sends CC 34 value 3 out to QC via MIDI
5. QC receives MIDI, changes to scene 3
6. QC sends back CC 34 value 3 (if configured to do so)
7. MIDIEngine receives CC event on background thread
   → decodes to MIDIEvent.sceneChanged(3)
   → publishes to AsyncStream
8. AppState task consumes sceneChanged event
   → updates currentSceneIndex = 3
9. SwiftUI observes @Observable change
   → SceneGridView re-renders, button 3 now highlighted
```

### Example 2: User Changes Preset on QC Hardware
```
1. User changes preset on QC physical buttons
2. QC sends Program Change message (e.g., PC 42)
3. CoreMIDI receives on background thread
4. MIDIEngine callback decodes PC 42
   → emits MIDIEvent.presetChanged(42, 0, 0)
   → publishes to AsyncStream
5. AppState task consumes event
   → calls PresetLibrary.preset(for: 42)
   → gets Preset with midiProgramNumber 42
   → updates currentPreset
   → clears isLastKnownState flag
6. SwiftUI observes changes
   → PresetHeaderView updates bank/slot and name
   → SceneGridView updates (scenes changed because preset changed)
   → orange "Last known" badge fades away
```

### Example 3: App Restarts
```
1. App launches
2. AppState.start() called
   → restores (programNumber: 42, sceneIndex: 2) from UserDefaults
   → sets currentPreset = presetLibrary.preset(for: 42)
   → sets currentSceneIndex = 2
   → sets isLastKnownState = true
3. UI renders with preset name + "Last known" badge
4. User or device changes anything
   → MIDI event arrives
   → isLastKnownState cleared
   → badge disappears
5. If user force-quits and the QC preset actually changed in the meantime:
   → App still shows old preset until next MIDI event
   → Once user taps any scene/changes anything, MIDI event updates the app to the correct state
```

---

## Threading Model

```
┌──────────────────────┐
│   CoreMIDI Callback  │
│   (background T1)    │
│                      │
│  Decodes MIDI bytes  │
│  → MIDIEvent         │
└───────────┬──────────┘
            │
            ▼
┌──────────────────────────────┐
│     MIDIEngine (Actor)       │
│   Executor: T1 (background)  │
│                              │
│  Publishes to AsyncStream    │
└───────────┬──────────────────┘
            │
            ▼
┌──────────────────────────────┐
│     AppState.start() Task    │
│    Executor: Main Thread     │
│                              │
│  for await event in stream   │
│    → update @Observable      │
└───────────┬──────────────────┘
            │
            ▼
┌──────────────────────────────┐
│     SwiftUI @Observable      │
│   (always on main thread)    │
│                              │
│  View re-renders             │
└──────────────────────────────┘
```

**Safety:** The `actor` boundary ensures that CoreMIDI events from background threads are isolated, then safely published to the main thread via `AsyncStream`.

---

## Persistence Strategy

| Data | Store | Why |
|---|---|---|
| Presets / Scenes | SwiftData | Authoritative library, queried on every MIDI event |
| Last known preset/scene | UserDefaults | Instant access on app launch, separate concern from library |
| MIDI config | UserDefaults | Small, frequent access |
| Cloud credentials | Keychain | Secure, not persisted to disk |
| Cloud auth tokens | Memory only | Temporary, cleared on sign out |

---

## Error Handling Strategy

**MIDI Engine Errors:**
- CoreMIDI setup failure → logged, app shows "MIDI not available" in UI
- Outgoing MIDI send fails → logged, but doesn't block other operations (user may retry)

**Provider Errors:**
- Invalid backup file → caught, shown to user in SettingsSheet
- Cloud API error → shown to user with retry option
- Network error → shown to user with network error message

**General:**
- No silent failures
- All async operations have error paths
- User is informed of sync status and any failures
