#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace qcbridge {

// Tracks the File-read stream (issue #60): after the handshake, one File
// READ gets the device streaming its whole catalog back as File UPDATE
// messages. Mirrors qc-mcp's transport.py::list_directory() - collect until
// the stream goes quiet for kQuietWindowMs, or kOverallTimeoutMs elapses
// overall, whichever comes first (about 400 messages over about 12s on a
// real device, per that function's docstring).
//
// Plain C++23, no Zephyr dependency - same reasoning as HidReassembler.hpp:
// the time-based state machine is unit-testable on its own by passing in
// synthetic `nowMs` values, with no real waiting and no kernel fixture
// needed. Not thread-safe by itself - QcSession calls every method under
// its own mutex, since onMessage()/onInflateFailed() land here from the
// dispatch/inflate worker threads while checkTimeout() runs from the
// heartbeat thread.
class QcFileStream {
public:
    // issue #33 (ADR 0007): was 1500ms. A hardware run (commit 147fb48)
    // found that too short - a genuine pause mid-reply, more than 1500ms
    // but still part of the same reply, looked like the stream ending, and
    // the real tail that followed got miscounted as a separate request's
    // reply. 5000ms (first used as this issue's own measurement window,
    // then kept through every subsequent hardware run with no recurrence of
    // that problem) is the value with actual evidence behind it.
    static constexpr int64_t kQuietWindowMs = 5000;
    static constexpr int64_t kOverallTimeoutMs = 30000;

    struct Summary {
        uint32_t messageCount = 0;
        uint32_t compressedCount = 0;
        uint32_t failedCount = 0;
        size_t totalWireBytes = 0;
        size_t totalInflatedBytes = 0;
        // Largest single message seen (successfully dispatched/inflated
        // ones only - see maxWireBytes/maxInflatedBytes member comments
        // below for why a message dropped for being too big never reaches
        // here at all, and so can understate the real peak). What #33's
        // buffer sizing actually needs. maxInflatedBytes is the one
        // exception to "inflated ones only": onInflateFailed() below folds
        // in a failed inflate's true size too, when its gzip trailer's
        // ISIZE field was readable (QcGzip.hpp's peekDecompressedSize()) -
        // that's a real, known inflated size even though the bytes
        // themselves were never produced.
        size_t maxWireBytes = 0;
        size_t maxInflatedBytes = 0;
        int64_t elapsedMs = 0;
        bool timedOut = false;  // true: hit kOverallTimeoutMs; false: went quiet

        // Not computed by this class - it has no visibility into
        // QcDispatcher/QcGzipInflater's mailboxes or QcHidBridge's
        // HidReassembler, all of which can silently drop a File message
        // before it ever reaches onMessage()/onInflateFailed() below (see
        // issue #60's "known gap"). QcSession fills these in from its own
        // snapshot-at-start/diff-at-finish of those classes' counters, so
        // the gap shows up here instead of only in scattered log lines.
        // Left at 0 by anything that doesn't know better (e.g. this
        // class's own tests).
        uint32_t dispatcherDropped = 0;
        uint32_t gzipDropped = 0;
        uint32_t reassemblyOverflowCount = 0;
        uint32_t reassemblyMalformedCount = 0;
    };

    // Called once, right after the handshake completes.
    void start(int64_t nowMs) {
        active_ = true;
        messageCount_ = 0;
        compressedCount_ = 0;
        failedCount_ = 0;
        totalWireBytes_ = 0;
        totalInflatedBytes_ = 0;
        maxWireBytes_ = 0;
        maxInflatedBytes_ = 0;
        startedAtMs_ = nowMs;
        lastMessageAtMs_ = nowMs;
    }

    bool isActive() const { return active_; }

    // A File message that decoded successfully (`compressed` says whether
    // it needed QcGzipInflater.hpp first). Ignored once the stream has
    // already finished (isActive() == false) - a late straggler shouldn't
    // reopen a summary that's already been logged.
    void onMessage(int64_t nowMs, size_t wireSize, size_t inflatedSize, bool compressed) {
        if (!active_) {
            return;
        }
        ++messageCount_;
        if (compressed) {
            ++compressedCount_;
        }
        totalWireBytes_ += wireSize;
        totalInflatedBytes_ += inflatedSize;
        maxWireBytes_ = std::max(maxWireBytes_, wireSize);
        maxInflatedBytes_ = std::max(maxInflatedBytes_, inflatedSize);
        lastMessageAtMs_ = nowMs;
    }

    // A File message whose gzip payload failed to inflate (QcGzip.hpp) -
    // e.g. it exceeded kMaxInflatedSize. Still real traffic: counts toward
    // the stream and resets the quiet window. totalInflatedBytes isn't
    // credited (no bytes were actually produced), but maxInflatedBytes_
    // still tracks `trueInflatedSize` when the caller could read it off the
    // gzip trailer without decompressing (QcGzipInflater.hpp /
    // QcGzip.hpp's peekDecompressedSize(), issue #33) - a real hardware
    // `RecallPreset` that was too big for kMaxInflatedSize is exactly why
    // this matters for sizing a real buffer.
    void onInflateFailed(int64_t nowMs, size_t wireSize,
                         std::optional<size_t> trueInflatedSize = std::nullopt) {
        if (!active_) {
            return;
        }
        ++messageCount_;
        ++failedCount_;
        totalWireBytes_ += wireSize;
        maxWireBytes_ = std::max(maxWireBytes_, wireSize);
        if (trueInflatedSize) {
            maxInflatedBytes_ = std::max(maxInflatedBytes_, *trueInflatedSize);
        }
        lastMessageAtMs_ = nowMs;
    }

    // Call periodically (QcSession's ~200ms heartbeat - see QcSession.hpp).
    // Returns a Summary and deactivates, exactly once, the moment either
    // deadline is hit; std::nullopt otherwise (including when not active).
    std::optional<Summary> checkTimeout(int64_t nowMs) {
        if (!active_) {
            return std::nullopt;
        }
        const bool quiet = messageCount_ > 0 && (nowMs - lastMessageAtMs_) >= kQuietWindowMs;
        const bool timedOut = (nowMs - startedAtMs_) >= kOverallTimeoutMs;
        if (!quiet && !timedOut) {
            return std::nullopt;
        }
        active_ = false;
        return Summary{
            .messageCount = messageCount_,
            .compressedCount = compressedCount_,
            .failedCount = failedCount_,
            .totalWireBytes = totalWireBytes_,
            .totalInflatedBytes = totalInflatedBytes_,
            .maxWireBytes = maxWireBytes_,
            .maxInflatedBytes = maxInflatedBytes_,
            .elapsedMs = nowMs - startedAtMs_,
            .timedOut = timedOut,
        };
    }

private:
    bool active_ = false;
    uint32_t messageCount_ = 0;
    uint32_t compressedCount_ = 0;
    uint32_t failedCount_ = 0;
    size_t totalWireBytes_ = 0;
    size_t totalInflatedBytes_ = 0;
    size_t maxWireBytes_ = 0;
    size_t maxInflatedBytes_ = 0;
    int64_t startedAtMs_ = 0;
    int64_t lastMessageAtMs_ = 0;
};

}  // namespace qcbridge
