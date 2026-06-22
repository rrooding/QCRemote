import Foundation

enum PresetLibraryError: Error {
    case invalidFile
    case corruptedData
    case networkError(String)
    case authenticationFailed
    case notImplemented
}
