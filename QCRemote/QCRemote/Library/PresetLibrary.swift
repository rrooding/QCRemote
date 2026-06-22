import Foundation
import SwiftData

enum SortOption {
    case bankAndSlot
    case name
    case syncDate
}

@MainActor @Observable
class PresetLibrary {
    let modelContext: ModelContext

    init(modelContext: ModelContext) {
        self.modelContext = modelContext
    }

    // MARK: - Sync

    func sync(from provider: PresetLibraryProvider) async throws {
        let incoming = try await provider.fetchLibrary()

        for preset in incoming {
            let programNumber = preset.midiProgramNumber
            if let existing = self.preset(for: programNumber) {
                existing.name = preset.name
                existing.bankIndex = preset.bankIndex
                existing.slotIndex = preset.slotIndex
                existing.lastSyncDate = Date()
                existing.syncSource = preset.syncSource

                existing.scenes.forEach { modelContext.delete($0) }
                existing.scenes = preset.scenes
            } else {
                modelContext.insert(preset)
            }
        }

        let metadata = lastSyncInfo(for: provider.id) ?? SyncMetadata(source: provider.id, displayName: provider.displayName)
        metadata.lastSyncDate = Date()
        metadata.displayName = provider.displayName
        if metadata.modelContext == nil {
            modelContext.insert(metadata)
        }

        try modelContext.save()
    }

    // MARK: - Lookup

    func preset(for programNumber: Int) -> Preset? {
        var descriptor = FetchDescriptor<Preset>(
            predicate: #Predicate { $0.midiProgramNumber == programNumber }
        )
        descriptor.fetchLimit = 1
        return try? modelContext.fetch(descriptor).first
    }

    // MARK: - Inventory

    func allPresets(sortedBy option: SortOption) -> [Preset] {
        let descriptor: FetchDescriptor<Preset>
        switch option {
        case .bankAndSlot:
            descriptor = FetchDescriptor<Preset>(sortBy: [
                SortDescriptor(\.bankIndex),
                SortDescriptor(\.slotIndex),
            ])
        case .name:
            descriptor = FetchDescriptor<Preset>(sortBy: [
                SortDescriptor(\.name),
            ])
        case .syncDate:
            descriptor = FetchDescriptor<Preset>(sortBy: [
                SortDescriptor(\.lastSyncDate, order: .reverse),
            ])
        }
        return (try? modelContext.fetch(descriptor)) ?? []
    }

    func preset(bank: Int, slot: Int) -> Preset? {
        var descriptor = FetchDescriptor<Preset>(
            predicate: #Predicate { $0.bankIndex == bank && $0.slotIndex == slot }
        )
        descriptor.fetchLimit = 1
        return try? modelContext.fetch(descriptor).first
    }

    // MARK: - Metadata

    func lastSyncInfo(for source: SyncSource) -> SyncMetadata? {
        var descriptor = FetchDescriptor<SyncMetadata>(
            predicate: #Predicate { $0.source == source }
        )
        descriptor.fetchLimit = 1
        return try? modelContext.fetch(descriptor).first
    }
}
