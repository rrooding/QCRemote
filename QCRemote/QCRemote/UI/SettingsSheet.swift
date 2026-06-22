import SwiftUI
import UniformTypeIdentifiers

struct SettingsSheet: View {
    @Environment(AppState.self) var appState
    @Environment(PresetLibrary.self) var presetLibrary
    @Environment(\.dismiss) var dismiss

    @State private var receiveChannel: Int = 1
    @State private var sceneChangeCC: Int = 34
    @State private var showFileImporter = false
    @State private var importError: String?

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
                    Button("Import Backup File (.qcb)") {
                        showFileImporter = true
                    }

                    if let lastSync = presetLibrary.lastSyncInfo(for: .backupFile) {
                        Text("Last sync: \(lastSync.displayName)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }

                if let error = importError {
                    Section {
                        Text(error)
                            .foregroundStyle(.red)
                    }
                }
            }
            .navigationTitle("Settings")
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") {
                        var config = MIDIConfiguration()
                        config.receiveChannel = receiveChannel
                        config.sceneChangeCC = sceneChangeCC
                        config.save()
                        dismiss()
                    }
                }
            }
            .fileImporter(
                isPresented: $showFileImporter,
                allowedContentTypes: [.archive],
                onCompletion: { result in
                    switch result {
                    case .success(let url):
                        let provider = BackupFileProvider(fileURL: url)
                        Task {
                            do {
                                try await presetLibrary.sync(from: provider)
                                importError = nil
                            } catch {
                                importError = "Import failed: \(error.localizedDescription)"
                            }
                        }
                    case .failure(let error):
                        importError = "File selection failed: \(error.localizedDescription)"
                    }
                }
            )
            .onAppear {
                let config = MIDIConfiguration.load()
                receiveChannel = config.receiveChannel
                sceneChangeCC = config.sceneChangeCC
            }
        }
    }
}
