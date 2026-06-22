import Foundation

struct MIDIConfiguration: Codable {
    var receiveChannel: Int = 1
    var sceneChangeCC: Int = 34
    var allChannels: Bool = false

    private static let userDefaultsKey = "MIDIConfiguration"

    func save() {
        if let data = try? JSONEncoder().encode(self) {
            UserDefaults.standard.set(data, forKey: Self.userDefaultsKey)
        }
    }

    static func load() -> MIDIConfiguration {
        guard let data = UserDefaults.standard.data(forKey: userDefaultsKey),
              let config = try? JSONDecoder().decode(MIDIConfiguration.self, from: data)
        else {
            return MIDIConfiguration()
        }
        return config
    }
}
