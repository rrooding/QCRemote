import Foundation

class CortexCloudProvider: PresetLibraryProvider {
    var id: SyncSource { .cortexCloud }
    var displayName: String { "Cortex Cloud" }

    func fetchLibrary() async throws -> [Preset] {
        // TODO: Implement once Cortex Cloud API is reverse-engineered
        throw PresetLibraryError.notImplemented
    }
}
