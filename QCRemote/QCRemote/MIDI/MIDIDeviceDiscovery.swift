import Foundation
import CoreMIDI

struct MIDIDeviceInfo: Identifiable, Hashable {
    let name: String
    let endpointRef: MIDIEndpointRef

    var id: String { name }
}

enum MIDIDeviceDiscovery {
    static func availableSources() -> [MIDIDeviceInfo] {
        (0..<MIDIGetNumberOfSources()).compactMap { i in
            let endpoint = MIDIGetSource(i)
            return deviceInfo(for: endpoint)
        }
    }

    static func availableDestinations() -> [MIDIDeviceInfo] {
        (0..<MIDIGetNumberOfDestinations()).compactMap { i in
            let endpoint = MIDIGetDestination(i)
            return deviceInfo(for: endpoint)
        }
    }

    private static func deviceInfo(for endpoint: MIDIEndpointRef) -> MIDIDeviceInfo? {
        var name: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(endpoint, kMIDIPropertyDisplayName, &name)
        guard let displayName = name?.takeRetainedValue() as String? else { return nil }
        return MIDIDeviceInfo(name: displayName, endpointRef: endpoint)
    }
}
