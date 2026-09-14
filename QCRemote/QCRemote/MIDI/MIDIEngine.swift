import Foundation
import CoreMIDI

actor MIDIEngine {
    private var midiClient: MIDIClientRef = 0
    private var inputPort: MIDIPortRef = 0
    private var outputPort: MIDIPortRef = 0
    private var configuration: MIDIConfiguration

    private var eventContinuation: AsyncStream<MIDIEvent>.Continuation?
    private(set) var events: AsyncStream<MIDIEvent>!

    init(configuration: MIDIConfiguration) {
        self.configuration = configuration
        self.events = AsyncStream { [weak self] continuation in
            Task { await self?.setEventContinuation(continuation) }
        }
    }

    private func setEventContinuation(_ continuation: AsyncStream<MIDIEvent>.Continuation) {
        self.eventContinuation = continuation
    }

    // MARK: - Lifecycle

    func start() async throws {
        var status = MIDIClientCreateWithBlock("QCRemote" as CFString, &midiClient) { [weak self] notification in
            Task { await self?.handleMIDINotification(notification) }
        }
        guard status == noErr else { throw MIDIError.initializationFailed }

        status = MIDIInputPortCreateWithProtocol(
            midiClient,
            "QCRemote Input" as CFString,
            ._1_0,
            &inputPort
        ) { [weak self] eventList, _ in
            let events = Self.parseEventList(eventList)
            Task { await self?.handleParsedEvents(events) }
        }
        guard status == noErr else { throw MIDIError.inputPortCreationFailed }

        status = MIDIOutputPortCreate(midiClient, "QCRemote Output" as CFString, &outputPort)
        guard status == noErr else { throw MIDIError.outputPortCreationFailed }

        connectSources()
    }

    func stop() {
        if inputPort != 0 {
            MIDIPortDispose(inputPort)
            inputPort = 0
        }
        if outputPort != 0 {
            MIDIPortDispose(outputPort)
            outputPort = 0
        }
        if midiClient != 0 {
            MIDIClientDispose(midiClient)
            midiClient = 0
        }
        eventContinuation?.finish()
    }

    // MARK: - Output

    func selectPreset(_ programNumber: Int) throws {
        let channel = UInt8(configuration.receiveChannel - 1) & 0x0F
        let status: UInt8 = 0xC0 | channel
        let data: UInt8 = UInt8(programNumber & 0x7F)
        try sendMessage([status, data])
    }

    func selectScene(_ index: Int) throws {
        let channel = UInt8(configuration.receiveChannel - 1) & 0x0F
        let status: UInt8 = 0xB0 | channel
        let cc = UInt8(configuration.sceneChangeCC & 0x7F)
        let value = UInt8(index & 0x7F)
        try sendMessage([status, cc, value])
    }

    // MARK: - Private

    func updateConfiguration(_ newConfig: MIDIConfiguration) {
        disconnectAllSources()
        configuration = newConfig
        connectSources()
    }

    private func connectSources() {
        let sources = MIDIDeviceDiscovery.availableSources()
        print("[MIDI] Found \(sources.count) MIDI source(s)")

        let selectedName = configuration.selectedSourceName
        let sourcesToConnect = selectedName != nil
            ? sources.filter { $0.name == selectedName }
            : sources

        for source in sourcesToConnect {
            print("[MIDI] Connecting to source: \(source.name)")
            MIDIPortConnectSource(inputPort, source.endpointRef, nil)
        }

        if !sourcesToConnect.isEmpty {
            eventContinuation?.yield(.connectionChanged(.connected))
        }
    }

    private func disconnectAllSources() {
        let sources = MIDIDeviceDiscovery.availableSources()
        for source in sources {
            MIDIPortDisconnectSource(inputPort, source.endpointRef)
        }
    }

    private func handleMIDINotification(_ notification: UnsafePointer<MIDINotification>) {
        switch notification.pointee.messageID {
        case .msgObjectAdded:
            print("[MIDI] Device added, reconnecting sources")
            connectSources()
        case .msgObjectRemoved:
            print("[MIDI] Device removed")
            let sources = MIDIDeviceDiscovery.availableSources()
            let selectedName = configuration.selectedSourceName
            let relevant = selectedName != nil
                ? sources.filter { $0.name == selectedName }
                : sources
            if relevant.isEmpty {
                eventContinuation?.yield(.connectionChanged(.disconnected))
            }
        default:
            break
        }
    }

    private func handleParsedEvents(_ events: [MIDIEvent]) {
        for event in events {
            eventContinuation?.yield(event)
        }
    }

    private nonisolated static func parseEventList(_ eventList: UnsafePointer<MIDIEventList>) -> [MIDIEvent] {
        var events: [MIDIEvent] = []
        let list = eventList.pointee
        withUnsafePointer(to: list.packet) { firstPacket in
            var packet = firstPacket
            for _ in 0..<list.numPackets {
                let words = UnsafeBufferPointer(
                    start: UnsafeRawPointer(packet).advanced(by: MemoryLayout<MIDIEventPacket>.offset(of: \MIDIEventPacket.words)!).assumingMemoryBound(to: UInt32.self),
                    count: Int(packet.pointee.wordCount)
                )
                for word in words {
                    if let event = parseMIDI1Word(word) {
                        events.append(event)
                    }
                }
                packet = UnsafePointer(MIDIEventPacketNext(packet))
            }
        }
        return events
    }

    private nonisolated static func parseMIDI1Word(_ word: UInt32) -> MIDIEvent? {
        let byte0 = UInt8((word >> 24) & 0xFF)
        let byte1 = UInt8((word >> 16) & 0xFF)
        let byte2 = UInt8((word >> 8) & 0xFF)

        let messageType = byte0 >> 4
        guard messageType == 0x2 else { return nil } // MIDI 1.0 channel voice

        let statusByte = byte1
        let messageKind = statusByte & 0xF0

        switch messageKind {
        case 0xC0: // Program Change
            return .presetChanged(programNumber: Int(byte2), bankMSB: 0, bankLSB: 0)
        case 0xB0: // Control Change
            let cc = byte2
            let value = UInt8((word) & 0xFF)
            // Scene change CC is checked by the consumer via configuration
            return .sceneChanged(index: Int(value))
        default:
            return nil
        }
    }

    private func sendMessage(_ bytes: [UInt8]) throws {
        let destinations = MIDIDeviceDiscovery.availableDestinations()
        let selectedName = configuration.selectedDestinationName
        let targets = selectedName != nil
            ? destinations.filter { $0.name == selectedName }
            : destinations

        guard !targets.isEmpty else { throw MIDIError.sendFailed }

        var eventList = MIDIEventList()
        var packet = MIDIEventListInit(&eventList, ._1_0)

        let word: UInt32
        if bytes.count == 2 {
            word = UInt32(0x20) << 24 | UInt32(bytes[0]) << 16 | UInt32(bytes[1]) << 8
        } else if bytes.count == 3 {
            word = UInt32(0x20) << 24 | UInt32(bytes[0]) << 16 | UInt32(bytes[1]) << 8 | UInt32(bytes[2])
        } else {
            throw MIDIError.sendFailed
        }

        packet = MIDIEventListAdd(&eventList, MemoryLayout<MIDIEventList>.size, packet, 0, 1, [word])

        for target in targets {
            let status = MIDISendEventList(outputPort, target.endpointRef, &eventList)
            if status != noErr { throw MIDIError.sendFailed }
        }
    }
}
