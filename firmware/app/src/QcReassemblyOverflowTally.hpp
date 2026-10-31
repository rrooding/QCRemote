#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace qcbridge {

// Per-command stats for HidReassembler::Status::Overflow (issue #33): a
// message whose true size exceeded QcHidBridge's kReassemblyCapacity, body
// dropped, but whose trailer survived (HidReassembler::overflowTrailer()),
// so the command id is still known via QcMessage.hpp's
// decodeTrailerCommand(). Written only from QcHidBridge::onReportIn() on
// Zephyr's `usbh` thread - lock-free (std::atomic, relaxed ordering, no
// mutex) for the same reason QcSession::recordReassemblyOverflow() already
// is: that thread's 1536-byte stack with CONFIG_LOG_MODE_IMMEDIATE means no
// logging and nothing that can block from there (see QcHidBridge.hpp's
// comment on the Overflow case).
//
// usbh is a single thread, so record() is never called concurrently with
// itself - the atomics here exist purely so the one concurrent reader (the
// heartbeat thread, logging a File stream's summary) doesn't invoke a data
// race, not to arbitrate between writers. A reader can see a slot
// mid-update (command just claimed, count not yet bumped) - a legitimate
// transient value, same tradeoff QcMessageTally's own header comment
// accepts for its own cross-thread read.
//
// Bounded like QcMessageTally: distinct commands beyond
// kMaxDistinctCommands fall into an "other" bucket rather than growing
// unboundedly - overflow is expected to be rare and concentrated on a
// handful of commands (large catalog/state replies like ModelRepo, or
// oversized File folder listings), not every command in the protocol.
//
// Lifetime counts, not scoped to one File stream the way QcFileStream's own
// dropped/overflow totals are (QcSession snapshots-and-diffs those at
// stream start/finish) - this firmware does one connect per boot, so
// since-boot and since-this-connect are the same thing in practice, and
// snapshotting a whole table of atomics without a lock to scope it further
// isn't worth the complexity for a diagnostic this rare (31 overflows across
// a whole connect's ~8800 IN transfers, on real hardware - issue #60's
// hardware run).
class QcReassemblyOverflowTally {
public:
    static constexpr size_t kMaxDistinctCommands = 16;

    // Reported when HidReassembler::overflowTrailer() didn't yield a full
    // trailer to decode a command from (QcMessage.hpp's
    // decodeTrailerCommand() returned std::nullopt) - should not happen in
    // practice, since Capacity is always far larger than a trailer, but a
    // defined bucket beats silently miscounting it as command 0.
    static constexpr uint16_t kUnknownCommand = 0xFFFE;

    struct Entry {
        uint16_t command = 0;
        uint32_t count = 0;
        uint32_t maxTrueSize = 0;
    };

    // Called from the usbh thread only - see class comment.
    void record(uint16_t command, size_t trueSize) {
        for (auto& slot : slots_) {
            uint16_t slotCommand = slot.command.load(std::memory_order_relaxed);
            if (slotCommand == kSlotEmpty) {
                slot.command.store(command, std::memory_order_relaxed);
                slotCommand = command;
            }
            if (slotCommand == command) {
                slot.count.fetch_add(1, std::memory_order_relaxed);
                bumpMax(slot.maxTrueSize, trueSize);
                return;
            }
        }
        otherCount_.fetch_add(1, std::memory_order_relaxed);
        bumpMax(otherMaxTrueSize_, trueSize);
    }

    // Snapshot for logging - called from the heartbeat thread (never from
    // usbh, so no write/read ordering requirement beyond the relaxed loads
    // below). Returns the number of entries written into `out`.
    size_t entries(std::array<Entry, kMaxDistinctCommands>& out) const {
        size_t n = 0;
        for (const auto& slot : slots_) {
            const uint16_t command = slot.command.load(std::memory_order_relaxed);
            if (command == kSlotEmpty) {
                continue;
            }
            out[n++] = Entry{.command = command,
                              .count = slot.count.load(std::memory_order_relaxed),
                              .maxTrueSize = slot.maxTrueSize.load(std::memory_order_relaxed)};
        }
        return n;
    }

    uint32_t otherCount() const { return otherCount_.load(std::memory_order_relaxed); }
    uint32_t otherMaxTrueSize() const { return otherMaxTrueSize_.load(std::memory_order_relaxed); }

private:
    static constexpr uint16_t kSlotEmpty = 0xFFFF;

    static void bumpMax(std::atomic<uint32_t>& slotMax, size_t value) {
        if (value > slotMax.load(std::memory_order_relaxed)) {
            slotMax.store(static_cast<uint32_t>(value), std::memory_order_relaxed);
        }
    }

    struct Slot {
        std::atomic<uint16_t> command{kSlotEmpty};
        std::atomic<uint32_t> count{0};
        std::atomic<uint32_t> maxTrueSize{0};
    };

    std::array<Slot, kMaxDistinctCommands> slots_{};
    std::atomic<uint32_t> otherCount_{0};
    std::atomic<uint32_t> otherMaxTrueSize_{0};
};

}  // namespace qcbridge
