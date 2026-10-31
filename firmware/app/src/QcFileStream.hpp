#pragma once

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
    static constexpr int64_t kQuietWindowMs = 1500;
    static constexpr int64_t kOverallTimeoutMs = 30000;

    struct Summary {
        uint32_t messageCount = 0;
        uint32_t compressedCount = 0;
        uint32_t failedCount = 0;
        size_t totalWireBytes = 0;
        size_t totalInflatedBytes = 0;
        int64_t elapsedMs = 0;
        bool timedOut = false;  // true: hit kOverallTimeoutMs; false: went quiet
    };

    // Called once, right after the handshake completes.
    void start(int64_t nowMs) {
        active_ = true;
        messageCount_ = 0;
        compressedCount_ = 0;
        failedCount_ = 0;
        totalWireBytes_ = 0;
        totalInflatedBytes_ = 0;
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
        lastMessageAtMs_ = nowMs;
    }

    // A File message whose gzip payload failed to inflate (QcGzip.hpp) -
    // e.g. it exceeded kMaxInflatedSize. Still real traffic: counts toward
    // the stream and resets the quiet window, just with no inflated size to
    // add (see "Why the sizes matter" in issue #60).
    void onInflateFailed(int64_t nowMs, size_t wireSize) {
        if (!active_) {
            return;
        }
        ++messageCount_;
        ++failedCount_;
        totalWireBytes_ += wireSize;
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
    int64_t startedAtMs_ = 0;
    int64_t lastMessageAtMs_ = 0;
};

}  // namespace qcbridge
