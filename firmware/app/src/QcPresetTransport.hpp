#pragma once

#include <cstdint>
#include <optional>

#include "QcCatalogSnapshot.hpp"

namespace qcbridge {

// The Current preset (GLOSSARY.md): the Catalog entry the Quad Cortex
// reports as loaded, by its Catalog-wide index. Empty when the Board
// doesn't know - before the Quad Cortex has reported a position, or when
// that position isn't in the Catalog.
using QcCurrentPreset = std::optional<QcCatalogSnapshot::PresetEntry>;

// The Link (GLOSSARY.md): the Board's USB connection to the Quad Cortex.
struct QcLinkState {
    bool qcConnected = false;
};

// The seam issue #71 asks for: a generic layer between QcSession/
// QcPresetCatalog (protocol/session logic, see QcSession.hpp) and whatever
// carries updates out to a client - Wi-Fi/HTTP today (QcWifiWebTransport.hpp),
// BLE later. Deliberately minimal - what a client actually shows (the
// Catalog, the Current preset and the Link, issue #99) - not any
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

    // Called whenever the Quad Cortex reports a preset position (a
    // SetlistPosition message), and again with each new Catalog, since the
    // same position can name a different Catalog entry after a re-read.
    // May repeat an unchanged value.
    virtual void publishCurrentPreset(const QcCurrentPreset& current) = 0;

    // Called when the Quad Cortex is claimed on USB or goes away.
    virtual void publishLink(const QcLinkState& link) = 0;
};

}  // namespace qcbridge
