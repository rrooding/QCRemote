#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <etl/string.h>

#include "QcHandshakeMessages.hpp"  // MessageAction, HandshakeMessageBuffer
#include "QcMessage.hpp"
#include "QcProtobuf.hpp"

namespace qcbridge {

// CortexMessageType.Enum command id for SetlistPosition (protocol.py's
// COMMANDS map; issue #5 Tier 2).
inline constexpr uint16_t kCommandSetlistPosition = 2;

// folder_key/key_in_downloads/key_in_plugin_folder are path-ish identifiers
// (e.g. "/media/p4/Presets/My Presets" or a user subfolder beneath it for
// folder_key; a shorter "cloud-<type>-<n>" style key for the other two - see
// qc-mcp's directory.py/DIRECTORY.md). No device-side max length is
// documented anywhere in qc-mcp; 96 is a provisional bound generous enough
// for every form observed in qc-mcp's docs, not a verified hardware limit -
// revisit against real device traffic alongside issue #33.
inline constexpr size_t kSetlistPathCapacity = 96;
using SetlistPathString = etl::string<kSetlistPathCapacity>;

// SetlistPositionMessage: 1=action, 2=request_id, 3=folder_key,
// 4=position, 5=is_factory, 6=is_downloads, 7=key_in_downloads, 8=is_plugin,
// 9=key_in_plugin_folder. Flat, scalar-only - verified against qc-mcp's
// descriptor set. See docs/adr/0004-tier2-protobuf-codec.md.
struct SetlistPositionReply {
    MessageAction action = MessageAction::Create;
    uint64_t requestId = 0;
    SetlistPathString folderKey;
    uint32_t position = 0;
    bool isFactory = false;
    bool isDownloads = false;
    SetlistPathString keyInDownloads;
    bool isPlugin = false;
    SetlistPathString keyInPluginFolder;
};

// Decodes a SetlistPositionMessage's protobuf bytes (the trailer already
// stripped by decodeMessage()). Fields absent from the stream keep the
// zero-value defaults above, matching proto3 presence semantics without
// needing to track has-bits.
inline std::optional<SetlistPositionReply> decodeSetlistPosition(std::span<const uint8_t> protoBytes) {
    if (protoBytes.empty()) {
        return std::nullopt;
    }

    SetlistPositionReply reply;
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
                reply.folderKey.assign(reinterpret_cast<const char*>(field->bytes.data()),
                                        field->bytes.size());
            }
            break;
        case 4:
            if (field->wireType == ProtoWireType::Varint) {
                reply.position = static_cast<uint32_t>(field->varintValue);
            }
            break;
        case 5:
            if (field->wireType == ProtoWireType::Varint) {
                reply.isFactory = field->varintValue != 0;
            }
            break;
        case 6:
            if (field->wireType == ProtoWireType::Varint) {
                reply.isDownloads = field->varintValue != 0;
            }
            break;
        case 7:
            if (field->wireType == ProtoWireType::LengthDelimited) {
                reply.keyInDownloads.assign(reinterpret_cast<const char*>(field->bytes.data()),
                                             field->bytes.size());
            }
            break;
        case 8:
            if (field->wireType == ProtoWireType::Varint) {
                reply.isPlugin = field->varintValue != 0;
            }
            break;
        case 9:
            if (field->wireType == ProtoWireType::LengthDelimited) {
                reply.keyInPluginFolder.assign(reinterpret_cast<const char*>(field->bytes.data()),
                                                field->bytes.size());
            }
            break;
        default:
            break;
        }
    }

    return reply;
}

// Loads the Preset at `slot` of the Folder keyed `folderKey` (issue #78):
// an UPDATE with folder_key and position, the shape qc-mcp's recall() sends
// for a user Folder. The Quad Cortex answers with a SetlistPosition push of
// where it actually is (onSetlistPosition() in QcSession.hpp).
//
// Worst case: a 96-byte key, a 10-byte request id and a slot under 256 come to
// 122 bytes with the trailer, inside the 128-byte buffer.
inline void encodeSetlistPositionUpdate(HandshakeMessageBuffer& out, uint64_t requestId, std::string_view folderKey,
                                        uint32_t slot) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Update));
    writeUInt64(proto, 2, requestId);
    writeString(proto, 3, folderKey);
    writeUInt64(proto, 4, slot);
    encodeMessage(out, kCommandSetlistPosition, {proto.data(), proto.size()});
}

}  // namespace qcbridge
