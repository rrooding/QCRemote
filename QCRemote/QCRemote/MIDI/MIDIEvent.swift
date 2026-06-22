import Foundation

enum MIDIEvent {
    case presetChanged(programNumber: Int, bankMSB: Int, bankLSB: Int)
    case sceneChanged(index: Int)
    case connectionChanged(MIDIConnectionStatus)
}

enum MIDIConnectionStatus {
    case connected
    case disconnected
}
