import Foundation

class BackupFileProvider: PresetLibraryProvider {
    let fileURL: URL
    var id: SyncSource { .backupFile }
    var displayName: String { fileURL.lastPathComponent }

    init(fileURL: URL) {
        self.fileURL = fileURL
    }

    func fetchLibrary() async throws -> [Preset] {
        // TODO: Implement once .qcb backup format is documented
        // 1. Read .qcb as ZIP archive
        // 2. Extract and parse JSON for preset/scene data
        // 3. Map to Preset/PresetScene objects
        throw PresetLibraryError.notImplemented
    }
}
