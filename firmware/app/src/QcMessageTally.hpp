#pragma once

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
// Plain C++23, no Zephyr dependency - same reasoning as QcFileStream.hpp:
// trivial to unit-test, and QcSession is what actually calls this under its
// own mutex (not thread-safe by itself).
//
// Bounded: distinct command ids beyond kMaxDistinctCommands fall into
// otherCount() rather than growing unboundedly - this is a short-lived
// debugging aid watching one connect's worth of traffic, not something
// that needs to handle an open-ended command space.
class QcMessageTally {
public:
    static constexpr size_t kMaxDistinctCommands = 8;

    struct Entry {
        uint16_t command = 0;
        uint32_t count = 0;
    };

    void start() {
        active_ = true;
        entryCount_ = 0;
        otherCount_ = 0;
        entries_ = {};
    }

    bool isActive() const { return active_; }

    // Ignored once stopped - same "don't reopen a finished window" contract
    // as QcFileStream::onMessage().
    void record(uint16_t command) {
        if (!active_) {
            return;
        }
        for (size_t i = 0; i < entryCount_; ++i) {
            if (entries_[i].command == command) {
                ++entries_[i].count;
                return;
            }
        }
        if (entryCount_ < entries_.size()) {
            entries_[entryCount_] = Entry{.command = command, .count = 1};
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
