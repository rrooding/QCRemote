#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <etl/vector.h>

namespace qcbridge {

// Every qc-mcp message = protobuf bytes + this 8-byte trailer:
//   [command: uint16 LE][reserved: uint32 = 0][hash: uint16]
// (protocol.py's encode_message/decode_message; see issue #5.)
inline constexpr size_t kMessageTrailerSize = 8;

// Appends `protoBytes` followed by the trailer for `command` to `out`. Hash
// is always 0 on host->QC requests (protocol.py's encode_message default -
// only replies carry a real payload_hash).
inline void encodeMessage(etl::ivector<uint8_t>& out, uint16_t command,
                          std::span<const uint8_t> protoBytes) {
    out.insert(out.end(), protoBytes.begin(), protoBytes.end());
    out.push_back(static_cast<uint8_t>(command & 0xFF));
    out.push_back(static_cast<uint8_t>((command >> 8) & 0xFF));
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
}

struct DecodedMessage {
    uint16_t command;
    std::span<const uint8_t> protoBytes;
    uint16_t hash;
};

// Splits a reassembled HID message into its protobuf payload and trailer.
// Does NOT gunzip - Tier 1 messages (issue #7's handshake) are never
// compressed. A caller that might receive a gzip'd payload (issue #6, e.g.
// ModelRepo) needs to inflate protoBytes itself before treating it as plain
// protobuf.
inline std::optional<DecodedMessage> decodeMessage(std::span<const uint8_t> fullMessage) {
    if (fullMessage.size() < kMessageTrailerSize) {
        return std::nullopt;
    }

    const size_t protoLen = fullMessage.size() - kMessageTrailerSize;
    const auto trailer = fullMessage.subspan(protoLen, kMessageTrailerSize);
    const auto command = static_cast<uint16_t>(trailer[0] | (trailer[1] << 8));
    const auto hash = static_cast<uint16_t>(trailer[6] | (trailer[7] << 8));

    return DecodedMessage{
        .command = command,
        .protoBytes = fullMessage.subspan(0, protoLen),
        .hash = hash,
    };
}

}  // namespace qcbridge
