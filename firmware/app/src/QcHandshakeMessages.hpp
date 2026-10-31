#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <etl/string.h>
#include <etl/vector.h>

#include "QcMessage.hpp"
#include "QcProtobuf.hpp"

namespace qcbridge {

// CortexMessageType.Enum command ids issue #7's handshake needs (qc-mcp's
// protocol.py COMMANDS map; full field shapes recorded in issue #5).
inline constexpr uint16_t kCommandVersion = 10;
inline constexpr uint16_t kCommandConnection = 49;
inline constexpr uint16_t kCommandResetCommsBuffers = 52;

// MessageAction.Enum values (protocol.py's ACTION dict).
enum class MessageAction : int {
    Create = 0,
    Update = 1,
    Delete = 2,
    Read = 3,
    Move = 4,
    Copy = 5,
    Upload = 6,
    Download = 7,
    Swap = 8,
};

// Largest encoded Tier 1 message (a Version UPDATE carrying a version
// string) plus the 8-byte trailer, rounded up - generous but bounded per
// CODING_STANDARDS' static-allocation rule, not sized "to be safe".
inline constexpr size_t kHandshakeMessageCapacity = 128;
using HandshakeMessageBuffer = etl::vector<uint8_t, kHandshakeMessageCapacity>;

// Long enough for a full git SHA (40 hex chars) or a semver-like release
// string, whichever the device's Version reply actually carries.
using FirmwareVersionString = etl::string<40>;

// ResetCommsBuffersMessage: 1=request_id, 2=session_id.
inline void encodeResetCommsBuffers(HandshakeMessageBuffer& out, std::string_view sessionId) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeUInt64(proto, 1, 0);
    writeString(proto, 2, sessionId);
    encodeMessage(out, kCommandResetCommsBuffers, {proto.data(), proto.size()});
}

// VersionMessage READ: 1=action only.
inline void encodeVersionRead(HandshakeMessageBuffer& out) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Read));
    encodeMessage(out, kCommandVersion, {proto.data(), proto.size()});
}

// VersionMessage UPDATE: 1=action, 2=request_id, 11=cortex_control_version.
inline void encodeVersionUpdate(HandshakeMessageBuffer& out, std::string_view cortexControlVersion,
                                uint64_t requestId) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Update));
    writeUInt64(proto, 2, requestId);
    writeString(proto, 11, cortexControlVersion);
    encodeMessage(out, kCommandVersion, {proto.data(), proto.size()});
}

// ConnectionMessage UPDATE: 1=request_id, 2=connected. No `action` field on
// this message - verified against qc-mcp's descriptor set (issue #5), not
// assumed to match the others.
inline void encodeConnection(HandshakeMessageBuffer& out, bool connected, uint64_t requestId) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeUInt64(proto, 1, requestId);
    writeBool(proto, 2, connected);
    encodeMessage(out, kCommandConnection, {proto.data(), proto.size()});
}

// Firmware version fields pulled from a Version READ reply. Field
// preference order (zenos_git_hash, then app_fw_version) mirrors qc-mcp's
// transport.py::_reported_firmware - announcing app_fw_version's build hash
// as our own cortex_control_version instead would fail the device's
// compatibility check.
struct VersionReply {
    FirmwareVersionString zenosGitHash;
    FirmwareVersionString appFwVersion;
};

inline std::optional<VersionReply> decodeVersionReply(std::span<const uint8_t> protoBytes) {
    if (protoBytes.empty()) {
        return std::nullopt;
    }

    VersionReply reply;
    ProtoFieldReader reader(protoBytes);

    while (const auto field = reader.next()) {
        if (field->wireType != ProtoWireType::LengthDelimited) {
            continue;
        }
        if (field->fieldNum == 4) {
            reply.zenosGitHash.assign(reinterpret_cast<const char*>(field->bytes.data()),
                                      field->bytes.size());
        } else if (field->fieldNum == 7) {
            reply.appFwVersion.assign(reinterpret_cast<const char*>(field->bytes.data()),
                                      field->bytes.size());
        }
    }

    return reply;
}

}  // namespace qcbridge
