#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace qcbridge {

// Diagnostic, added during issue #60's hardware debugging: the File-read
// stream's hook only ever looked at command == kCommandFile, so a run where
// the summary showed zero File messages couldn't tell "the device sent
// nothing at all" apart from "the device sent something, just not File" -
// every other command's traffic during the window was invisible. This
// tallies every message actually handed to QcSession::onMessage()/
// onInflateFailed() by command id, regardless of what it is, while active.
//
// Also tracks, per command, the largest wire size and inflated size seen
// (issue #33's sizing exercise: QcFileStream.hpp's own maxWireBytes/
// maxInflatedBytes are useful totals across the whole stream, but #33 needs
// to know which specific command produced the peak, not just that some
// command did). record()'s size parameters default to 0 so a caller that
// only cares about counts (this class's own existing tests) doesn't need to
// pass them.
//
// Plain C++23, no Zephyr dependency - same reasoning as QcFileStream.hpp:
// trivial to unit-test, and QcSession is what actually calls this under its
// own mutex (not thread-safe by itself).
//
// Bounded: distinct command ids beyond kMaxDistinctCommands fall into
// otherCount() rather than growing unboundedly - this is a short-lived
// debugging aid watching one connect's worth of traffic, not something
// that needs to handle an open-ended command space.
//
// Sized from a real hardware run (issue #60/#61, commit 0d1edcc): in one
// connect's worth of traffic this firmware can see the 12 post-connect
// state-READ replies (QcStateReads.hpp), File (4), Version (10), and
// preset-change pushes (RecallPreset 15, Scene 13, Grid 1,
// RecentsFavorites 20, PresetDirty 34, SetlistPosition 2) - 18 distinct
// ids observed or enumerable in practice. A static 8-slot cap measured
// smaller than that on hardware: state-READ replies alone filled every
// slot before File's own first message arrived, so File's ~550-message
// stream was counted entirely as "other" instead. 24 leaves headroom
// above the 18 known ids without tracking any one command specially.
class QcMessageTally {
public:
    static constexpr size_t kMaxDistinctCommands = 24;

    struct Entry {
        uint16_t command = 0;
        uint32_t count = 0;
        size_t maxWireBytes = 0;
        size_t maxInflatedBytes = 0;
    };

    void start() {
        active_ = true;
        entryCount_ = 0;
        otherCount_ = 0;
        entries_ = {};
    }

    bool isActive() const { return active_; }

    // Ignored once stopped - same "don't reopen a finished window" contract
    // as QcFileStream::onMessage(). `wireSize`/`inflatedSize` feed the
    // per-command peak-size tracking (issue #33) - leave at 0 if unknown or
    // not relevant to the caller (e.g. no decoded size at all).
    void record(uint16_t command, size_t wireSize = 0, size_t inflatedSize = 0) {
        if (!active_) {
            return;
        }
        for (size_t i = 0; i < entryCount_; ++i) {
            if (entries_[i].command == command) {
                ++entries_[i].count;
                entries_[i].maxWireBytes = std::max(entries_[i].maxWireBytes, wireSize);
                entries_[i].maxInflatedBytes = std::max(entries_[i].maxInflatedBytes, inflatedSize);
                return;
            }
        }
        if (entryCount_ < entries_.size()) {
            entries_[entryCount_] = Entry{.command = command,
                                          .count = 1,
                                          .maxWireBytes = wireSize,
                                          .maxInflatedBytes = inflatedSize};
            ++entryCount_;
            return;
        }
        ++otherCount_;
    }

    // Call once the window is over; stops further recording until the next
    // start(). Deliberately separate from QcFileStream's own
    // quiet-window/overall-timeout logic - this just mirrors whatever
    // window QcSession decides to run it over.
    void stop() { active_ = false; }

    std::span<const Entry> entries() const { return {entries_.data(), entryCount_}; }
    uint32_t otherCount() const { return otherCount_; }

private:
    bool active_ = false;
    std::array<Entry, kMaxDistinctCommands> entries_{};
    size_t entryCount_ = 0;
    uint32_t otherCount_ = 0;
};

}  // namespace qcbridge
