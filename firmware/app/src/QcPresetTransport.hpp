#pragma once

#include <cstdint>

#include "QcCatalogSnapshot.hpp"

namespace qcbridge {

// A preset-change notification forwarded from QcSession's own post-connect
// tally (QcMessageTally.hpp) - issue #71's "live update when a preset
// change arrives, for example the active command events the firmware
// already sees". Deliberately just the command id and its running count,
// not decoded preset content: a transport that wants the new state calls
// back into the session's own catalog (publishCatalog(), fired right
// alongside this for the commands that are actually preset-list-affecting)
// rather than this event carrying protocol-level detail a BLE transport
// would have no use for either.
struct QcPresetChangeEvent {
    uint16_t command = 0;
    uint32_t count = 0;
};

// The seam issue #71 asks for: a generic layer between QcSession/
// QcPresetCatalog (protocol/session logic, see QcSession.hpp) and whatever
// carries updates out to a client - Wi-Fi/HTTP today (QcWifiWebTransport.hpp),
// BLE later. Deliberately minimal - the two things a client actually needs
// (the current preset list, and a signal that it changed) - not any
// HTTP/BLE-specific shape, so this header pulls in nothing beyond plain
// C++ and QcCatalogSnapshot.hpp. QcSession depends on this interface, never
// on a concrete transport; a concrete transport depends on this interface
// plus whatever its own medium needs (sockets, GATT, ...), never on
// QcSession or QcPresetCatalog directly - QcSession hands it only the
// snapshot/event types below.
//
// QcSession calls these synchronously from its own heartbeat/dispatcher
// threads and they must never block or do real work inline - same contract
// as QcPresetPrinter::requestPrint(): an implementation that needs to do
// anything beyond a fast copy/enqueue (serializing JSON, writing to a
// socket) must hand off to a thread of its own. Multiple transports can be
// registered at once (QcSession::addTransport()) so Wi-Fi and a future BLE
// transport can run side by side, or the build can enable just one -
// issue #71's "allow running Wi-Fi and BLE side by side, or letting the
// user choose which is active" requirement.
class QcPresetTransport {
public:
    virtual ~QcPresetTransport() = default;

    // Called once a File-read stream finishes (QcSession::logFileStreamSummary())
    // and whenever a reconnect produces a fresh listing. `snapshot` is only
    // valid for the duration of this call - an implementation that needs it
    // longer must copy it into its own storage.
    virtual void publishCatalog(const QcCatalogSnapshot& snapshot) = 0;

    // Called from QcSession::runHeartbeat() whenever its post-stream tally
    // sees a new or bumped command - see that function's own comment for
    // why that's already throttled before reaching here.
    virtual void publishPresetChange(const QcPresetChangeEvent& event) = 0;
};

}  // namespace qcbridge
