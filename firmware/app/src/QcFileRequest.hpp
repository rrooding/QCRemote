#pragma once

#include <cstdint>

#include <etl/vector.h>

#include "QcHandshakeMessages.hpp"
#include "QcMessage.hpp"
#include "QcProtobuf.hpp"

namespace qcbridge {

// FileMessage's CortexMessageType.Enum id (qc-mcp's protocol.py COMMANDS
// map). Kept separate from QcHandshakeMessages.hpp's command ids since File
// isn't part of the handshake.
inline constexpr uint16_t kCommandFile = 4;

// FileMessage READ: 1=action, 2=request_id, 11=omit_factory_content
// (verified against qc-mcp's own descriptor set, qc_descriptors-4.1.pb,
// same way issue #5's Tier 2 messages were). A READ with no `type`/`folder`
// set asks for the device's whole catalog (presets, IRs, captures) as a
// stream of File UPDATE messages - mirrors qc-mcp's own
// transport.py::list_directory(), except qc-mcp itself never sets
// `omit_factory_content`.
//
// Always true (issue #33): a hardware measurement (commit 219cf6e, see
// docs/adr/0007-file-reassembly-buffer-sizing.md) found this is the option
// that actually shrinks the catalog - from 568 File messages/~716KB down
// to 20/~30KB on the owner's unit - while still naming every one of their
// 48 user/cloud presets (`My Presets`, `Practice`, `cloud-0-1`); a sibling
// option, `short_listing`, measured no effect at all. What gets left out
// is factory-bundled plugin-library content (e.g. "Archetype: Cory Wong
// X"'s own presets) - still fully present on the device, just not listed
// here. A `short_listing`-style toggle isn't exposed as a parameter
// anymore: this is the one shape the firmware ever sends.
inline void encodeFileRead(HandshakeMessageBuffer& out, uint64_t requestId) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Read));
    writeUInt64(proto, 2, requestId);
    writeBool(proto, 11, true);
    encodeMessage(out, kCommandFile, {proto.data(), proto.size()});
}

}  // namespace qcbridge
