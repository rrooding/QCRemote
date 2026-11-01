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

// FileMessage READ's optional short_listing (6) / omit_factory_content (11)
// fields - verified against qc-mcp's own descriptor set
// (qc_descriptors-4.1.pb) for issue #33's variant experiment
// (QcFileReadExperiment.hpp). qc-mcp's own source never sets either field
// (checked directly, not assumed), so what they actually do to the device's
// listing is unknown until tested on hardware. Both default to false, which
// keeps encodeFileRead() below byte-for-byte identical to its pre-#33 shape
// (neither field is written when false - see encodeFileRead()'s comment).
struct FileReadOptions {
    bool shortListing = false;
    bool omitFactoryContent = false;
};

// FileMessage READ: 1=action, 2=request_id, plus optionally 6=short_listing
// and/or 11=omit_factory_content (verified against qc-mcp's protobuf
// descriptor set, same way issue #5's Tier 2 messages were). A bare READ
// with no `type`/`folder` set asks for the device's whole catalog (presets,
// IRs, captures) as a stream of File UPDATE messages - mirrors qc-mcp's
// transport.py::list_directory(). See QcFileStream.hpp for how that stream
// is collected; the sibling issue's QcFileMessage.hpp (issue #59, wired in
// by #10) is what eventually decodes a reply's payload - this file only
// encodes the request.
//
// `options` defaults to both fields unset, which proto2 omits entirely
// rather than sending a false - so a default-argument call produces the
// exact same bytes this function always has, and existing callers don't
// need to change.
inline void encodeFileRead(HandshakeMessageBuffer& out, uint64_t requestId,
                           FileReadOptions options = {}) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Read));
    writeUInt64(proto, 2, requestId);
    if (options.shortListing) {
        writeBool(proto, 6, true);
    }
    if (options.omitFactoryContent) {
        writeBool(proto, 11, true);
    }
    encodeMessage(out, kCommandFile, {proto.data(), proto.size()});
}

}  // namespace qcbridge
