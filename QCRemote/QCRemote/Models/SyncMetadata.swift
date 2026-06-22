import Foundation
import SwiftData

@Model
final class SyncMetadata {
    var source: SyncSource
    var lastSyncDate: Date
    var displayName: String

    init(
        source: SyncSource,
        lastSyncDate: Date = Date(),
        displayName: String
    ) {
        self.source = source
        self.lastSyncDate = lastSyncDate
        self.displayName = displayName
    }
}
