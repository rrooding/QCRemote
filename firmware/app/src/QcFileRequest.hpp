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

// FileMessage READ: 1=action, 2=request_id (verified against qc-mcp's
// protobuf descriptor set, same way issue #5's Tier 2 messages were). A bare
// READ with no `type`/`folder` set asks for the device's whole catalog
// (presets, IRs, captures) as a stream of File UPDATE messages - mirrors
// qc-mcp's transport.py::list_directory(). See QcFileStream.hpp for how that
// stream is collected; the sibling issue's QcFileMessage.hpp (issue #59,
// wired in by #10) is what eventually decodes a reply's payload - this file
// only encodes the request.
inline void encodeFileRead(HandshakeMessageBuffer& out, uint64_t requestId) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Read));
    writeUInt64(proto, 2, requestId);
    encodeMessage(out, kCommandFile, {proto.data(), proto.size()});
}

}  // namespace qcbridge
