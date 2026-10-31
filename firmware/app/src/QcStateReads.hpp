#pragma once

#include <array>
#include <cstdint>

#include <etl/vector.h>

#include "QcHandshakeMessages.hpp"
#include "QcMessage.hpp"
#include "QcModelRepoMessage.hpp"        // kCommandModelRepo
#include "QcProtobuf.hpp"
#include "QcSetlistPositionMessage.hpp"  // kCommandSetlistPosition

namespace qcbridge {

// CortexMessageType.Enum command ids for issue #61's post-connect state
// READs (qc-mcp's protocol.py COMMANDS map). ModelRepo and SetlistPosition
// already have their own command-id constants from issue #5's Tier 2
// decoders (QcModelRepoMessage.hpp/QcSetlistPositionMessage.hpp) - reused
// below rather than redefined.
inline constexpr uint16_t kCommandModuleStats = 35;
inline constexpr uint16_t kCommandUndoRedo = 21;
inline constexpr uint16_t kCommandIOSettings = 3;
inline constexpr uint16_t kCommandGeneralSettings = 9;
inline constexpr uint16_t kCommandMode = 14;
inline constexpr uint16_t kCommandGlobalEQ = 38;
inline constexpr uint16_t kCommandMasterVolume = 17;
inline constexpr uint16_t kCommandGlobalTempo = 33;
inline constexpr uint16_t kCommandScene = 13;
inline constexpr uint16_t kCommandPresetDirty = 34;

// Exact order and set qc-mcp's transport.py::_handshake() sends these in,
// right after its Connection UPDATE and before any listing - its own
// comment on that step says plainly that this is what "makes the QC start
// streaming and marks the session active" (issue #61). Every one of these
// twelve message types shares the same two leading fields (1=action,
// 2=request_id), verified directly against qc-mcp's protobuf descriptor
// set (qc_descriptors-4.1.pb), not assumed from the Tier 1/File shape - so
// one generic encoder below (encodeStateRead()) covers all of them.
inline constexpr std::array<uint16_t, 12> kPostConnectStateReadCommands = {
    kCommandModelRepo,       kCommandModuleStats, kCommandUndoRedo,    kCommandIOSettings,
    kCommandGeneralSettings, kCommandMode,         kCommandGlobalEQ,    kCommandMasterVolume,
    kCommandGlobalTempo,     kCommandScene,        kCommandPresetDirty, kCommandSetlistPosition,
};

// A bare READ for one of the message types above: 1=action, 2=request_id,
// nothing else - the same shape qc-mcp's loop builds for each of them (its
// per-message object only ever gets those two fields assigned before
// sending). Reuses HandshakeMessageBuffer: every encoded message here is a
// handful of bytes plus the 8-byte trailer, far under that buffer's
// capacity.
inline void encodeStateRead(HandshakeMessageBuffer& out, uint16_t command, uint64_t requestId) {
    etl::vector<uint8_t, kHandshakeMessageCapacity> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Read));
    writeUInt64(proto, 2, requestId);
    encodeMessage(out, command, {proto.data(), proto.size()});
}

}  // namespace qcbridge
