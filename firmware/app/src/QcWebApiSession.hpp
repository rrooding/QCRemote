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
// Switching (issue #78) is checked here too: a switchPreset the Board
// accepts is handed out by takeSwitchRequest(), and the session holds it
// in flight until the Quad Cortex reports that Preset as the Current one,
// or the 4 s give-up passes, or nothing can confirm it any more (the Link
// went down, the Catalog or the page changed).
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

    // docs/api/README.md, "Switching a Preset": the page gives up after 4 s,
    // and the Board within the same 4 s.
    static constexpr int64_t kSwitchTimeoutMs = 4000;

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
    // A switch it had in flight is dropped too: the next page didn't ask
    // for it, and shouldn't be refused as busy because of it.
    void connectionClosed() {
        open_ = false;
        switchInFlight_.reset();
    }

    // A Catalog change is always a snapshot, never a patch.
    std::optional<std::string_view> catalogPublished(const QcCatalogSnapshot& catalog, int64_t nowMs) {
        encodeCatalogJson(catalog, catalogJson_);
        catalogGeneration_ = catalog.generation;
        catalogPresetCount_ = catalog.presetCount;
        // QcSession drops a request from a replaced Catalog, so nothing
        // will confirm it.
        switchInFlight_.reset();
        return open_ ? snapshot(nowMs) : std::optional<std::string_view>{};
    }

    // The Quad Cortex reporting the Preset a switch asked for completes
    // it, and that report is always sent, changed or not: the page clears
    // its pending state on it. Any other report (a footswitch at the unit,
    // the re-publish after a listing) leaves the switch in flight.
    std::optional<std::string_view> currentPresetChanged(const QcCurrentPreset& current, int64_t nowMs) {
        const bool completesSwitch = switchInFlight_ && current && current->index == switchInFlight_->index;
        if (completesSwitch) {
            switchInFlight_.reset();
        }
        if (!completesSwitch && sameCurrent(current, current_)) {
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
        if (!link_.qcConnected) {
            // The Quad Cortex that would have confirmed it is gone.
            switchInFlight_.reset();
            pendingSwitch_.reset();
        }
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
        return switchRequested(std::get<QcWebApiSwitchPreset>(request), nowMs);
    }

    // The switch accepted since the last call, for the Quad Cortex - once.
    std::optional<QcSwitchRequest> takeSwitchRequest() {
        const auto request = pendingSwitch_;
        pendingSwitch_.reset();
        return request;
    }

    // Liveness (docs/api/README.md): a tick once nothing else has gone out
    // for the tick interval.
    std::optional<std::string_view> timePassed(int64_t nowMs) {
        if (switchInFlight_ && nowMs >= switchInFlight_->deadlineMs) {
            switchInFlight_.reset();
        }
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
    // The checks in the order docs/api/README.md's "Errors" gives: no Quad
    // Cortex makes the rest moot, and an index only means something in its
    // own generation.
    std::optional<std::string_view> switchRequested(const QcWebApiSwitchPreset& request, int64_t nowMs) {
        etl::vector<char, 48> message;
        if (!link_.qcConnected) {
            return error(request.id, QcWebApiErrorCode::QcDisconnected, "link down", nowMs);
        }
        if (request.generation != catalogGeneration_) {
            detail::appendLiteral("generation ", message);
            detail::appendUInt(request.generation, message);
            detail::appendLiteral(" != ", message);
            detail::appendUInt(catalogGeneration_, message);
            return error(request.id, QcWebApiErrorCode::StaleCatalog, view(message), nowMs);
        }
        if (request.index >= catalogPresetCount_) {
            detail::appendLiteral("no preset ", message);
            detail::appendUInt(request.index, message);
            detail::appendLiteral(" in ", message);
            detail::appendUInt(static_cast<uint32_t>(catalogPresetCount_), message);
            return error(request.id, QcWebApiErrorCode::BadIndex, view(message), nowMs);
        }
        if (switchInFlight_) {
            return error(request.id, QcWebApiErrorCode::Busy, "a switch is in flight", nowMs);
        }
        pendingSwitch_ = QcSwitchRequest{.generation = request.generation, .index = request.index};
        switchInFlight_ = SwitchInFlight{.index = request.index, .deadlineMs = nowMs + kSwitchTimeoutMs};
        return std::nullopt;
    }

    template <size_t N>
    static std::string_view view(const etl::vector<char, N>& text) {
        return {text.data(), text.size()};
    }

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
    uint32_t catalogGeneration_ = 0;
    size_t catalogPresetCount_ = 0;
    std::optional<QcSwitchRequest> pendingSwitch_;
    struct SwitchInFlight {
        uint32_t index;
        int64_t deadlineMs;
    };
    std::optional<SwitchInFlight> switchInFlight_;
    etl::vector<char, kCatalogJsonCapacity> catalogJson_;
    Frame frame_;
};

}  // namespace qcbridge
