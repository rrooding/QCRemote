import Foundation
import SwiftData

@Model
final class Preset {
    var id: UUID
    var name: String
    var bankIndex: Int
    var slotIndex: Int
    var midiProgramNumber: Int
    @Relationship(deleteRule: .cascade, inverse: \PresetScene.preset)
    var scenes: [PresetScene]
    var lastSyncDate: Date
    var syncSource: SyncSource

    init(
        id: UUID = UUID(),
        name: String,
        bankIndex: Int,
        slotIndex: Int,
        midiProgramNumber: Int,
        scenes: [PresetScene] = [],
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
