#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include <etl/vector.h>

#include "QcCatalogJson.hpp"
#include "QcCatalogSnapshot.hpp"
#include "QcPresetTransport.hpp"
#include "QcWebApiMessages.hpp"
#include "QcWebApiRequest.hpp"

namespace qcbridge {

// The web API contract (docs/api/, ADR 0012) for one page's Connection,
// with no sockets in it: events in (the Catalog was published, the Current
// preset or the Link changed, a frame arrived, time passed), frames out.
// It owns seq, snapshot-vs-patch, ticks, the build id, inbound parsing and
// errors. QcWifiWebTransport.hpp only moves the frames, and tests/
// web_api_session drives this directly.
//
// It also keeps the Board state a snapshot needs (Catalog, Current preset,
// Link) across Connections, so a page that connects later still gets all of
// it. Each method returns the frame to send, if any - a view into this
// object's own buffer, valid until the next call. Not thread-safe: one
// thread drives it.
class QcWebApiSession {
public:
    // A snapshot is the Catalog JSON plus the envelope around it: type,
    // seq, a build id of at most 40 characters, and the current and link
    // sections (a 32-character name escapes to at most 64). 256 covers the
    // worst case of all of those with room to spare.
    static constexpr size_t kCatalogJsonCapacity = 16384;
    static constexpr size_t kEnvelopeOverhead = 256;
    using Frame = etl::vector<char, kCatalogJsonCapacity + kEnvelopeOverhead>;

    // The Catalog before the first listing: what encodeCatalogJson() makes
    // of an empty, incomplete snapshot (tests/web_api_session checks that),
    // written out because a QcCatalogSnapshot is too big to build on the
    // stack just for this.
    static constexpr std::string_view kEmptyCatalogJson =
        R"({"generation":0,"presetCount":0,"incomplete":true,"folders":[]})";

    // `build` must outlive this object (in the firmware it's a generated
    // constant, see cmake/EmbedWebAssets.cmake).
    QcWebApiSession(std::string_view build, int64_t tickIntervalMs)
        : build_(build), tickIntervalMs_(tickIntervalMs) {
        detail::appendLiteral(kEmptyCatalogJson, catalogJson_);
    }

    // A page's WebSocket opened: it gets a snapshot right away.
    std::optional<std::string_view> connectionOpened(int64_t nowMs) {
        open_ = true;
        return snapshot(nowMs);
    }

    // The page's WebSocket is gone. Board state is kept for the next one.
    void connectionClosed() { open_ = false; }

    // A Catalog change is always a snapshot, never a patch.
    std::optional<std::string_view> catalogPublished(const QcCatalogSnapshot& catalog, int64_t nowMs) {
        encodeCatalogJson(catalog, catalogJson_);
        return open_ ? snapshot(nowMs) : std::optional<std::string_view>{};
    }

    std::optional<std::string_view> currentPresetChanged(const QcCurrentPreset& current, int64_t nowMs) {
        if (sameCurrent(current, current_)) {
            return std::nullopt;
        }
        current_ = current;
        if (!open_) {
            return std::nullopt;
        }
        encodeCurrentPatchMessage(nextSeq_++, current_, frame_);
        return sent(nowMs);
    }

    std::optional<std::string_view> linkChanged(const QcLinkState& link, int64_t nowMs) {
        if (link.qcConnected == link_.qcConnected) {
            return std::nullopt;
        }
        link_ = link;
        if (!open_) {
            return std::nullopt;
        }
        encodeLinkPatchMessage(nextSeq_++, link_, frame_);
        return sent(nowMs);
    }

    // One text frame from the page. `frameLength` is its full length, which
    // exceeds text.size() when the transport had to cut it short.
    std::optional<std::string_view> frameReceived(std::string_view text, size_t frameLength, int64_t nowMs) {
        if (!open_) {
            return std::nullopt;
        }
        const QcWebApiRequest request = parseWebApiRequest(text, frameLength);
        if (std::holds_alternative<QcWebApiResync>(request)) {
            return snapshot(nowMs);
        }
        if (const auto* bad = std::get_if<QcWebApiBadRequest>(&request)) {
            return error(bad->id, QcWebApiErrorCode::BadRequest, bad->reason, nowMs);
        }
        // Executing a switch is issue #78. Until then a well-formed one is
        // refused, so the page's pending state clears instead of timing out.
        const auto& switchPreset = std::get<QcWebApiSwitchPreset>(request);
        return error(switchPreset.id, QcWebApiErrorCode::BadRequest, "not implemented", nowMs);
    }

    // Liveness (docs/api/README.md): a tick once nothing else has gone out
    // for the tick interval.
    std::optional<std::string_view> timePassed(int64_t nowMs) {
        if (!open_ || nowMs - lastSentMs_ < tickIntervalMs_) {
            return std::nullopt;
        }
        encodeTickMessage(nextSeq_++, frame_);
        return sent(nowMs);
    }

    // How long until timePassed() would send a tick (0 when one is due
    // already), or nothing without a page. A transport that waits no longer
    // than this between calls keeps the contract's "a message at least
    // every tick interval" exact instead of late by its own poll period.
    std::optional<int64_t> msUntilTickDue(int64_t nowMs) const {
        if (!open_) {
            return std::nullopt;
        }
        const int64_t remaining = lastSentMs_ + tickIntervalMs_ - nowMs;
        return remaining > 0 ? remaining : 0;
    }

private:
    static bool sameCurrent(const QcCurrentPreset& a, const QcCurrentPreset& b) {
        if (a.has_value() != b.has_value()) {
            return false;
        }
        return !a || (a->index == b->index && a->name == b->name);
    }

    std::optional<std::string_view> error(const std::optional<QcWebApiCommandId>& id, QcWebApiErrorCode code,
                                          std::string_view message, int64_t nowMs) {
        std::optional<std::string_view> idText;
        if (id) {
            idText = std::string_view(id->data(), id->size());
        }
        encodeErrorMessage(nextSeq_++, idText, code, message, frame_);
        return sent(nowMs);
    }

    std::optional<std::string_view> snapshot(int64_t nowMs) {
        encodeSnapshotMessage(build_, std::string_view(catalogJson_.data(), catalogJson_.size()), current_,
                              link_, frame_);
        nextSeq_ = 1;
        return sent(nowMs);
    }

    std::string_view sent(int64_t nowMs) {
        lastSentMs_ = nowMs;
        return {frame_.data(), frame_.size()};
    }

    std::string_view build_;
    int64_t tickIntervalMs_;
    bool open_ = false;
    uint32_t nextSeq_ = 0;
    int64_t lastSentMs_ = 0;
    QcCurrentPreset current_;
    QcLinkState link_;
    etl::vector<char, kCatalogJsonCapacity> catalogJson_;
    Frame frame_;
};

}  // namespace qcbridge
