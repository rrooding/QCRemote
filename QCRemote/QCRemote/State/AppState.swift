import Foundation
import SwiftData

@MainActor @Observable
class AppState {
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
        restoreLastKnownState()

        do {
            try await midiEngine.start()
        } catch {
            print("Failed to start MIDI engine: \(error)")
        }

        Task {
            for await event in await midiEngine.events {
                self.onMIDIEvent(event)
            }
        }
    }

    // MARK: - User Actions

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

    func reloadMIDIConfiguration() async {
        let config = MIDIConfiguration.load()
        await midiEngine.updateConfiguration(config)
    }

    // MARK: - Event Handling

    private func onMIDIEvent(_ event: MIDIEvent) {
        switch event {
        case .presetChanged(let pc, let bankMSB, let bankLSB):
            print("[MIDI] Preset changed: PC=\(pc) bankMSB=\(bankMSB) bankLSB=\(bankLSB)")
            if let preset = presetLibrary.preset(for: pc) {
                print("[MIDI] Matched preset: \(preset.name) (\(preset.bankAndSlot))")
                currentPreset = preset
                isLastKnownState = false
                persistLastKnownState()
            } else {
                print("[MIDI] No preset found for program number \(pc)")
            }
        case .sceneChanged(let index):
            print("[MIDI] Scene changed: index=\(index)")
            currentSceneIndex = index
            persistLastKnownState()
        case .connectionChanged(let status):
            print("[MIDI] Connection: \(status == .connected ? "connected" : "disconnected")")
            midiConnectionStatus = status
        }
    }

    // MARK: - Last Known State

    private func persistLastKnownState() {
        guard let preset = currentPreset else { return }
        UserDefaults.standard.set(preset.midiProgramNumber, forKey: "lastKnownProgramNumber")
        UserDefaults.standard.set(currentSceneIndex, forKey: "lastKnownSceneIndex")
    }

    private func restoreLastKnownState() {
        let programNumber = UserDefaults.standard.integer(forKey: "lastKnownProgramNumber")
        let sceneIndex = UserDefaults.standard.integer(forKey: "lastKnownSceneIndex")

        guard UserDefaults.standard.object(forKey: "lastKnownProgramNumber") != nil else { return }

        currentPreset = presetLibrary.preset(for: programNumber)
        currentSceneIndex = sceneIndex
        isLastKnownState = true
    }
}
