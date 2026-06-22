# Phase 1 — Xcode Project Setup

## Objective
Create a new Xcode project targeting iOS 17+/macOS 14+, set up folder structure, and configure SwiftData `ModelContainer`.

## Steps

### 1. Create New Xcode Project

1. Open **Xcode**
2. **File → New → Project**
3. Choose **iOS** template
4. Select **App** template
5. Fill in project options:
   - **Product Name:** `QCRemote`
   - **Team:** Your team (or None if not in a team)
   - **Organization Identifier:** `com.yourname` (e.g., `com.izerion`)
   - **Interface:** SwiftUI
   - **Language:** Swift
   - **Storage:** SwiftData
   - **Uncheck**: Core Data, Include Tests (we'll add tests later)

6. Choose location: `/home/ralph/Work/Personal/QCRemote` (or create a new folder for it)
   - Xcode will create a subfolder for the project

7. **Create**

---

### 2. Verify iOS and macOS Targets

After project creation:

1. Select the **QCRemote** project in the left sidebar
2. Select the **QCRemote** target
3. **General tab:**
   - **Minimum Deployments:**
     - iOS: 17.0
     - macOS: 14.0 (if adding macOS target later)

4. **Build Settings:**
   - Search for `SWIFT_VERSION` → should be 5.10 or later

---

### 3. Add macOS Target (Optional but Recommended)

If you want to also run on macOS (not just iOS):

1. **File → New → Target**
2. Choose **macOS** → **App**
3. Product Name: `QCRemote` (same)
4. Interface: SwiftUI
5. Storage: SwiftData
6. Team: same as iOS target
7. **Finish**

Xcode will create a separate target. You'll now have:
- `QCRemote` (iOS target)
- `QCRemote (Mac)` or similar (macOS target)

Both targets will share the same source files (Models, MIDI, Library, etc.) if organized correctly.

---

### 4. Organize Folder Structure in Xcode

Xcode created a default `QCRemote.swift` file. We'll reorganize into modules.

**Create folders in Xcode** (these appear as "groups" in Xcode, but can also be real folders on disk):

1. Right-click **QCRemote** folder in left sidebar
2. **New → Group** → name it **Models**
3. Repeat for: **MIDI**, **Library**, **Providers**, **State**, **UI**

Your sidebar should now look like:
```
QCRemote
├── QCRemoteApp.swift    (main app entry point)
├── Models
├── MIDI
├── Library
├── Providers
├── State
├── UI
└── ...
```

---

### 5. Optional: Create Matching Folders on Disk

For better organization, create actual folders on disk (not just Xcode groups):

```bash
cd /path/to/QCRemote/QCRemote/  # inside the project folder
mkdir Models MIDI Library Providers State UI Tests
```

Then in Xcode, when you create a file, select the corresponding group and Xcode will place it in that disk folder automatically.

---

### 6. Set Up SwiftData ModelContainer

SwiftData integration may already be scaffolded if you selected "SwiftData" during project creation. Verify:

1. Open **QCRemoteApp.swift** (the `@main` entry point)
2. Look for `ModelContainer` initialization:

```swift
@main
struct QCRemoteApp: App {
    let modelContainer: ModelContainer

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .modelContainer(modelContainer)
    }

    init() {
        let schema = Schema([
            // Models will go here
        ])
        let modelConfiguration = ModelConfiguration(
            schema: schema,
            isStoredInMemoryOnly: false
        )

        do {
            modelContainer = try ModelContainer(for: schema, configurations: [modelConfiguration])
        } catch {
            fatalError("Could not initialize ModelContainer: \(error)")
        }
    }
}
```

If the boilerplate is incomplete, fill it in as above. We'll add the actual `@Model` classes in Phase 2.

---

### 7. Create Placeholder Files (Optional)

To prevent Xcode warnings about empty groups, create placeholder `.swift` files:

**In each folder group:**
1. Right-click the group → **New → File**
2. Choose **Swift File**
3. Name it after the module (e.g., `Placeholder.swift` or `MIDIEngine.swift`)
4. Add a placeholder comment:

```swift
// Models module
// Preset, Scene, SyncMetadata models will go here
```

---

### 8. Verify Build

1. Select **QCRemote** scheme (iOS simulator)
2. Choose a simulator (e.g., iPhone 15 Pro)
3. **Product → Build** (or Cmd+B)

Should build without errors.

---

### 9. Update .gitignore

If this project is in a git repo, ensure `.gitignore` has:

```
.DS_Store
*.swiftpm/
xcuserdata/
*.xcworkspace/xcuserdata/
DerivedData/
Build/
.swiftpm/
```

---

## Checklist

- [ ] Xcode project created with SwiftUI and SwiftData
- [ ] iOS 17+ and macOS 14+ minimum deployments set
- [ ] (Optional) macOS target added
- [ ] Folder structure created: Models, MIDI, Library, Providers, State, UI
- [ ] ModelContainer initialized in QCRemoteApp.swift
- [ ] Project builds without errors
- [ ] (Optional) git repository initialized with proper .gitignore

## Next Steps

Once Phase 1 is complete:

1. Verify the project structure is correct
2. Move on to **Phase 2: Data Models** (see `docs/PHASE-2-onwards.md`)
3. As you create files, place them in the appropriate folder/group

## Notes

- **Targets:** If you created both iOS and macOS targets, both will build the same code. The folder/group structure is shared.
- **File references:** When you create a file in Xcode and assign it to a group, make sure the file is also physically placed in the corresponding folder (Xcode's default behavior).
- **Linking:** Both targets should link against Foundation and SwiftData frameworks automatically.
