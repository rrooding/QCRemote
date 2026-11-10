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

// The command ids this file's own header comment already calls out as
// "preset-change pushes" (RecallPreset, Scene, Grid, RecentsFavorites,
// PresetDirty, SetlistPosition) - the device's own signal that the loaded
// preset's identity or content changed, as opposed to a continuously
// streaming value like GlobalTempo/MasterVolume. Scene (13), PresetDirty
// (34) and SetlistPosition (2) already have named constants elsewhere
// (QcStateReads.hpp, QcSetlistPositionMessage.hpp) in this same namespace -
// given by value below, not by including those headers (this file would
// then fail to compile wherever both end up included in one translation
// unit, e.g. QcSession.hpp), so only the three ids with no existing
// constant get one here.
inline constexpr uint16_t kCommandGrid = 1;
inline constexpr uint16_t kCommandRecallPreset = 15;
inline constexpr uint16_t kCommandRecentsFavorites = 20;

inline constexpr std::array<uint16_t, 6> kPresetChangeCommands = {
    kCommandGrid, kCommandRecallPreset, /* Scene */ 13, kCommandRecentsFavorites,
    /* PresetDirty */ 34, /* SetlistPosition */ 2,
};

// True for a command id that marks the loaded preset's identity or content
// as having changed, false for a continuously-streaming one (GlobalTempo)
// or anything else this firmware tracks. Pure and Zephyr-free, directly
// unit-testable (tests/live_updates).
inline constexpr bool isPresetChangeCommand(uint16_t command) {
    for (const uint16_t candidate : kPresetChangeCommands) {
        if (candidate == command) {
            return true;
        }
    }
    return false;
}

// Issue #75's root-cause fix for "a preset change doesn't reach the page
// within a couple of seconds": QcSession::runHeartbeat() used to gate every
// *repeat* bump of every tracked command behind one shared
// kPostStreamLogIntervalMs window, so once GlobalTempo's own ~4/s ticking
// claimed that window (its first bump, or any later one that happened to
// land when the window was open), a real preset switch arriving a few
// seconds later had to wait for the same shared window to reopen - up to a
// minute later. That's exactly the 5068403 smoke test's "command=33 count=2"
// -> "count=170" a minute apart, and the page's own "preset change (cmd 33,
// ...)" mislabeling of a GlobalTempo tick.
//
// A brand-new command (isNew) was always forwarded immediately regardless -
// that part was already correct (it's the actual preset-change signal on a
// fresh connect). This extends "always forwarded immediately" to every
// repeat bump of a preset-change command too, since those are rare and
// user-triggered, never a reason to wait for the same window a 4/s stream
// needs. A continuously-streaming command's own throttling is unchanged.
inline constexpr bool shouldForwardTallyEntry(bool isNew, bool bumped, bool intervalDue,
                                              uint16_t command) {
    return isNew || (bumped && (intervalDue || isPresetChangeCommand(command)));
}

}  // namespace qcbridge
