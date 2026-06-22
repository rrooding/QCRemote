//
//  QCRemoteApp.swift
//  QCRemote
//
//  Created by Ralph Rooding on 22/06/2026.
//

import SwiftUI
import SwiftData

@main
struct QCRemoteApp: App {
    var sharedModelContainer: ModelContainer = {
        let schema = Schema([
            Preset.self,
            PresetScene.self,
            SyncMetadata.self,
        ])
        let modelConfiguration = ModelConfiguration(schema: schema, isStoredInMemoryOnly: false)

        do {
            return try ModelContainer(for: schema, configurations: [modelConfiguration])
        } catch {
            fatalError("Could not create ModelContainer: \(error)")
        }
    }()

    @State private var appState: AppState?

    var body: some Scene {
        WindowGroup {
            if let appState {
                ContentView()
                    .environment(appState)
                    .environment(appState.presetLibrary)
                    .task {
                        await appState.start()
                    }
            } else {
                ProgressView("Starting...")
                    .task {
                        let library = PresetLibrary(modelContext: sharedModelContainer.mainContext)
                        let engine = MIDIEngine(configuration: .load())
                        appState = AppState(midiEngine: engine, presetLibrary: library)
                    }
            }
        }
        .modelContainer(sharedModelContainer)
    }
}
