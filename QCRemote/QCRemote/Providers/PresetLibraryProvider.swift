import Foundation

protocol PresetLibraryProvider {
    var id: SyncSource { get }
    var displayName: String { get }
    func fetchLibrary() async throws -> [Preset]
}
