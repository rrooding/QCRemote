import Foundation

enum SyncSource: String, Codable {
    case backupFile
    case cortexCloud
    case usbDirect

    var displayName: String {
        switch self {
        case .backupFile: "Backup File"
        case .cortexCloud: "Cortex Cloud"
        case .usbDirect: "USB Direct"
        }
    }
}
