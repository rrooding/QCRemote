import Foundation

enum MIDIError: Error {
    case initializationFailed
    case inputPortCreationFailed
    case outputPortCreationFailed
    case sendFailed
}
