#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "QcHandshakeMessages.hpp"  // MessageAction
#include "QcProtobuf.hpp"

namespace qcbridge {

// CortexMessageType.Enum command id for ModelRepo (protocol.py's COMMANDS map;
// issue #5 Tier 2).
inline constexpr uint16_t kCommandModelRepo = 51;

// ModelRepoMessage: 1=action, 2=request_id, 3=model_repo_payload (bytes).
// Verified against qc-mcp's descriptor set - flat and scalar-only, same shape
// as Tier 1's messages. The "device catalog" nesting ADR 0003 expected here
// turned out to live entirely *inside* model_repo_payload (a gzip+tar of
// ModelRepo.xml - qc-mcp's PROTOCOL.md section 10 / catalog.py), not in this
// message's own protobuf structure. See docs/adr/0004-tier2-protobuf-codec.md.
struct ModelRepoReply {
    MessageAction action = MessageAction::Create;
    uint64_t requestId = 0;

    // Still gzip+tar'd - this decoder does not inflate it (issue #6 is what
    // turns this into ModelRepo.xml). Deliberately a span into the caller's
    // buffer rather than a copy: the full catalog (633 devices on CorOS 4.1)
    // is far too large to duplicate on top of the HID reassembly buffer that
    // already holds it - see issue #33's static-memory budget.
    std::span<const uint8_t> payload;
};

// Decodes a ModelRepoMessage's protobuf bytes (the trailer already stripped
// by decodeMessage()). Does not touch `payload`'s contents - real device
// traffic carries a compressed catalog there that needs #6's inflate step
// before it's anything but opaque bytes.
inline std::optional<ModelRepoReply> decodeModelRepo(std::span<const uint8_t> protoBytes) {
    if (protoBytes.empty()) {
        return std::nullopt;
    }

    ModelRepoReply reply;
    ProtoFieldReader reader(protoBytes);

    while (const auto field = reader.next()) {
        switch (field->fieldNum) {
        case 1:
            if (field->wireType == ProtoWireType::Varint) {
                reply.action = static_cast<MessageAction>(field->varintValue);
            }
            break;
        case 2:
            if (field->wireType == ProtoWireType::Varint) {
                reply.requestId = field->varintValue;
            }
            break;
        case 3:
            if (field->wireType == ProtoWireType::LengthDelimited) {
                reply.payload = field->bytes;
            }
            break;
        default:
            break;
        }
    }

    return reply;
}

}  // namespace qcbridge
