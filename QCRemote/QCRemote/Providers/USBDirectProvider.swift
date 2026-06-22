import Foundation

class USBDirectProvider: PresetLibraryProvider {
    var id: SyncSource { .usbDirect }
    var displayName: String { "USB Direct" }

    func fetchLibrary() async throws -> [Preset] {
        throw PresetLibraryError.notImplemented
    }
}
