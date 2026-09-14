import SwiftUI
import UniformTypeIdentifiers

struct SettingsSheet: View {
    @Environment(AppState.self) var appState
    @Environment(PresetLibrary.self) var presetLibrary
    @Environment(\.dismiss) var dismiss

    @State private var receiveChannel: Int = 1
    @State private var sceneChangeCC: Int = 34
    @State private var selectedSource: String = ""
    @State private var selectedDestination: String = ""
    @State private var availableSources: [MIDIDeviceInfo] = []
    @State private var availableDestinations: [MIDIDeviceInfo] = []
    @State private var showFileImporter = false
    @State private var importError: String?

    private static let allDevicesValue = ""

    var body: some View {
        NavigationStack {
            Form {
                Section("MIDI Device") {
                    Picker("Input", selection: $selectedSource) {
                        Text("All devices").tag(Self.allDevicesValue)
                        ForEach(availableSources) { source in
                            Text(source.name).tag(source.name)
                        }
                    }

                    Picker("Output", selection: $selectedDestination) {
                        Text("All devices").tag(Self.allDevicesValue)
                        ForEach(availableDestinations) { dest in
                            Text(dest.name).tag(dest.name)
                        }
                    }
                }

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
                        config.selectedSourceName = selectedSource.isEmpty ? nil : selectedSource
                        config.selectedDestinationName = selectedDestination.isEmpty ? nil : selectedDestination
                        config.save()
                        Task { await appState.reloadMIDIConfiguration() }
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
                selectedSource = config.selectedSourceName ?? Self.allDevicesValue
                selectedDestination = config.selectedDestinationName ?? Self.allDevicesValue
                availableSources = MIDIDeviceDiscovery.availableSources()
                availableDestinations = MIDIDeviceDiscovery.availableDestinations()
            }
        }
    }
}
