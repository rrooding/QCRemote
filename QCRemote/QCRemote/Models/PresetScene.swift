import Foundation
import SwiftData

@Model
final class PresetScene {
    var id: UUID
    var name: String
    var index: Int
    var preset: Preset?

    init(
        id: UUID = UUID(),
        name: String,
        index: Int,
        preset: Preset? = nil
    ) {
        self.id = id
        self.name = name
        self.index = index
        self.preset = preset
    }
}
