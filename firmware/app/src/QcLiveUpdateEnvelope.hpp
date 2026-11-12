#pragma once

#include <cstdint>
#include <string_view>

#include <etl/vector.h>

#include "QcCatalogJson.hpp"
#include "QcPresetTransport.hpp"

namespace qcbridge {

// Issue #75's WebSocket envelope: the subset of ADR 0011's
// {"v":1,"t":"<type>","seq":N,...} shape this issue needs - snapshot, patch
// and tick on the way out, resync on the way in. The full message schema is
// #15; this firmware only ever emits the three message types below and only
// ever recognizes one inbound one.
//
// Plain C++23 appenders over caller-provided, fixed-capacity storage (no
// heap - CODING_STANDARDS.md), same style and the same detail::append*
// helpers as QcCatalogJson.hpp's encodeCatalogJson(), and directly
// unit-testable the same way (tests/live_updates).
inline constexpr uint32_t kLiveUpdateProtocolVersion = 1;

namespace detail {

template <size_t N>
inline void appendEnvelopePrefix(std::string_view type, uint32_t seq, etl::vector<char, N>& out) {
    out.clear();
    appendLiteral("{\"v\":", out);
    appendUInt(kLiveUpdateProtocolVersion, out);
    appendLiteral(",\"t\":\"", out);
    appendLiteral(type, out);
    appendLiteral("\",\"seq\":", out);
    appendUInt(seq, out);
}

}  // namespace detail

// {"v":1,"t":"snapshot","seq":N,"catalog":<catalogJson>}
//
// Sent whenever the catalog itself changed (a fresh File-stream listing)
// and to every newly-connected client, so it always carries the full,
// current listing - a client that missed messages (a seq gap, or simply
// wasn't connected yet) can always get back to a known state from one of
// these, never a patch.
template <size_t N, size_t M>
inline void encodeSnapshotEnvelope(uint32_t seq, const etl::vector<char, M>& catalogJson,
                                   etl::vector<char, N>& out) {
    detail::appendEnvelopePrefix("snapshot", seq, out);
    detail::appendLiteral(",\"catalog\":", out);
    detail::appendLiteral(std::string_view(catalogJson.data(), catalogJson.size()), out);
    detail::appendLiteral("}", out);
}

// {"v":1,"t":"patch","seq":N,"lastChange":{"command":N,"count":M}}
//
// Sent when a preset-change event arrived but the catalog itself didn't
// change - a few dozen bytes instead of resending the whole listing (ADR
// 0011's "keep messages small", since the MCU can't compress WebSocket
// frames). The full #15 schema has more patchable sections; this is the
// one #75 needs.
template <size_t N>
inline void encodePatchEnvelope(uint32_t seq, const QcPresetChangeEvent& event, etl::vector<char, N>& out) {
    detail::appendEnvelopePrefix("patch", seq, out);
    detail::appendLiteral(",\"lastChange\":{\"command\":", out);
    detail::appendUInt(event.command, out);
    detail::appendLiteral(",\"count\":", out);
    detail::appendUInt(event.count, out);
    detail::appendLiteral("}}", out);
}

// {"v":1,"t":"tick","seq":N}
//
// Liveness only (ADR 0011 / issue #15): without this, the board is
// silent whenever nothing changes, and a half-open socket after a Wi-Fi
// drop looks identical to an idle rig. Sent on a cadence
// (QcWifiWebTransport's kTickIntervalMs) whenever nothing else was sent in
// that window - never required by the client (full reconnect-on-missed-
// tick logic is issue #20).
template <size_t N>
inline void encodeTickEnvelope(uint32_t seq, etl::vector<char, N>& out) {
    detail::appendEnvelopePrefix("tick", seq, out);
    detail::appendLiteral("}", out);
}

// The one inbound message this issue needs: {"v":1,"t":"resync"}, sent by
// the page's own JS (web/ui/src/store.js, issue #88 - originally
// QcWifiWebTransport.hpp's inline kIndexHtml, removed by that issue) after
// it detects a seq gap. Deliberately a substring check, not a JSON parser -
// this firmware only ever needs to recognize its own client's one
// outbound message shape, serialized exactly that way by a fixed call to
// JSON.stringify(), not arbitrary client input.
inline bool isResyncMessage(std::string_view text) {
    return text.find(R"("t":"resync")") != std::string_view::npos;
}

}  // namespace qcbridge
