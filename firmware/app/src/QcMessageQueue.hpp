#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace qcbridge {

// Bounded single-producer/single-consumer hand-off of whole decoded
// messages, from Zephyr's internal `usbh` thread to a worker thread
// (QcDispatcher.hpp). Issue #94's root-cause fix: the one-slot mailbox
// QcDispatcher.hpp used to carry dropped every message that arrived while
// its consumer hadn't been scheduled yet, and on this firmware's thread
// layout the producer *always* wins that race:
//
//   * `usbh` runs at K_PRIO_COOP(9) (subsys/usb/host/usbh_core.c), i.e.
//     priority -7 and cooperative, so it is scheduled ahead of every
//     thread this firmware owns and is never preempted by one.
//   * the consumer is an ordinary preemptible thread (priority 6).
//
// So any stretch where the consumer doesn't get a turn - a CPU-bound
// gzip::inflate() of a 14-23 KB RecallPreset at the same priority, a
// cooperative net/HTTP-server thread (both K_PRIO_COOP(15) = -1) handling
// Wi-Fi traffic, a synchronous CONFIG_LOG_MODE_IMMEDIATE log line - is a
// stretch where `usbh` keeps delivering messages into a slot that can
// only hold one. A deeper queue is the only structural answer: raising
// the consumer's priority can't fix it on its own, because -7 cooperative
// beats any priority a preemptible consumer can be given.
//
// Plain C++23 and <atomic> only, no Zephyr dependency - same reasoning as
// QcFileStream.hpp/QcMessageTally.hpp: directly unit-testable
// (tests/dispatch_queue), with QcDispatcher.hpp supplying the thread and
// the signalling semaphore around it. CODING_STANDARDS.md's
// time-sensitive-code rule asks for exactly this shape on the `usbh`
// side: a fixed-capacity ring plus a semaphore used only to signal, never
// a mutex the producer has to block on.
//
// Layout: a descriptor ring of `MaxRecords` slots over one shared byte
// arena of `ArenaCapacity`. Keeping payloads in a single arena rather than
// giving each slot its own worst-case buffer is what makes the depth
// affordable - a message's protobuf payload can be as large as a whole
// reassembled HID message (ADR 0007's 128 KiB File worst case), and
// `MaxRecords` buffers that size would be hundreds of KB of RAM. A record
// is always stored contiguously (the consumer hands a std::span straight
// into the arena to its sink, no second copy), so a record that would
// straddle the end of the arena is placed at offset 0 instead and the
// skipped tail is charged to it as padding.
//
// Both ends are lock-free and wait-free. Producer-owned state
// (writeSeq_, bytesClaimed_, writeOffset_, the counters) is only ever
// written by push(); consumer-owned state (readSeq_, bytesReleased_) only
// by pop(). The sequence numbers are monotonic, not modular, so "how many
// records are in flight" is a plain subtraction with no ambiguous
// full/empty state and no shared count for the two sides to fight over.
template <size_t ArenaCapacity, size_t MaxRecords>
class QcMessageQueue {
public:
    static_assert(ArenaCapacity > 0, "arena must hold at least one byte");
    static_assert(MaxRecords > 0, "queue must hold at least one record");

    struct Record {
        uint16_t command = 0;
        size_t wireSize = 0;
        // A view into the arena, valid until pop() retires this record.
        std::span<const uint8_t> protoBytes;
    };

    static constexpr size_t kArenaCapacity = ArenaCapacity;
    static constexpr size_t kMaxRecords = MaxRecords;

    // Producer side. Returns false (and counts a drop) when the queue is
    // full or `protoBytes` can never fit the arena at all. Does no
    // allocation, takes no lock and cannot block - safe on the `usbh`
    // thread, where a single bounded copy is the whole budget.
    bool push(uint16_t command, std::span<const uint8_t> protoBytes, size_t wireSize) {
        if (protoBytes.size() > ArenaCapacity) {
            // Permanent, not transient: no amount of draining makes room.
            // Counted apart from a full queue so a log can tell "this
            // message needs a bigger arena (or issue #66's streaming
            // decoder)" from "the consumer fell behind a burst".
            oversizedCount_.fetch_add(1, std::memory_order_relaxed);
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        const uint32_t length = static_cast<uint32_t>(protoBytes.size());

        const uint32_t writeSeq = writeSeq_.load(std::memory_order_relaxed);
        const uint32_t recordsInFlight = writeSeq - readSeq_.load(std::memory_order_acquire);
        if (recordsInFlight >= MaxRecords) {
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const uint32_t claimed = bytesClaimed_.load(std::memory_order_relaxed);
        const uint32_t released = bytesReleased_.load(std::memory_order_acquire);
        // Rewind to the start of an empty arena. Without this, the largest
        // record the queue can take depends on where the write offset
        // happens to sit: a record needing the whole arena only fits at
        // offset 0, since the skipped tail (below) is charged on top of its
        // own length. That would make an exactly-arena-sized message - ADR
        // 0007's 128 KiB worst-case `File` folder - a coin flip, where the
        // single slot this queue replaced always took it. Safe from the
        // producer alone: with nothing in flight no consumer holds a span
        // into the arena, and only the producer can make it non-empty.
        if (recordsInFlight == 0 && claimed == released) {
            writeOffset_ = 0;
        }

        // A record never straddles the end of the arena - see this class's
        // header comment for why the consumer needs it contiguous.
        uint32_t offset = writeOffset_;
        uint32_t padding = 0;
        if (offset + length > ArenaCapacity) {
            padding = static_cast<uint32_t>(ArenaCapacity) - offset;
            offset = 0;
        }

        const uint32_t bytesInFlight = claimed - released;
        const uint32_t needed = padding + length;
        if (needed > static_cast<uint32_t>(ArenaCapacity) - bytesInFlight) {
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::copy(protoBytes.begin(), protoBytes.end(), arena_.begin() + offset);
        slots_[writeSeq % MaxRecords] = Slot{.offset = offset,
                                             .length = length,
                                             .padding = padding,
                                             .wireSize = static_cast<uint32_t>(wireSize),
                                             .command = command};

        writeOffset_ = offset + length;
        if (writeOffset_ == ArenaCapacity) {
            writeOffset_ = 0;
        }

        // Issue #94's sizing evidence: both high-water marks are what turns
        // "how big is a real preset-change burst on this device" from a
        // guess into a measurement, reported once per increase by
        // QcSession::checkTrafficHealth() rather than per message.
        recordHighWater(maxRecordsInFlight_, recordsInFlight + 1);
        recordHighWater(maxBytesInFlight_, bytesInFlight + needed);

        bytesClaimed_.store(claimed + needed, std::memory_order_release);
        // Publishes the slot and the arena bytes it points at - must be last.
        writeSeq_.store(writeSeq + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. The oldest record still in the queue, or nullopt when
    // it's empty. The returned span stays valid until pop().
    std::optional<Record> front() const {
        const uint32_t readSeq = readSeq_.load(std::memory_order_relaxed);
        if (readSeq == writeSeq_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        const Slot& slot = slots_[readSeq % MaxRecords];
        return Record{.command = slot.command,
                      .wireSize = slot.wireSize,
                      .protoBytes = {arena_.data() + slot.offset, slot.length}};
    }

    // Retires the record front() returned, freeing its arena bytes. No-op
    // on an empty queue.
    void pop() {
        const uint32_t readSeq = readSeq_.load(std::memory_order_relaxed);
        if (readSeq == writeSeq_.load(std::memory_order_acquire)) {
            return;
        }
        const Slot& slot = slots_[readSeq % MaxRecords];
        bytesReleased_.store(bytesReleased_.load(std::memory_order_relaxed) + slot.padding + slot.length,
                             std::memory_order_release);
        readSeq_.store(readSeq + 1, std::memory_order_release);
    }

    // Lifetime count of messages push() refused - a full queue and an
    // oversized message combined, since both mean "this message never
    // reached the sink". Monotonic, never reset: callers wanting a count
    // scoped to one stream (QcFileStream.hpp) snapshot this at the start
    // and diff at the end.
    uint32_t droppedCount() const { return droppedCount_.load(std::memory_order_relaxed); }

    // The subset of droppedCount() that was too large for the arena
    // outright, rather than arriving while it was full.
    uint32_t oversizedCount() const { return oversizedCount_.load(std::memory_order_relaxed); }

    // Peak occupancy since boot, in records and in arena bytes.
    uint32_t maxRecordsInFlight() const { return maxRecordsInFlight_.load(std::memory_order_relaxed); }
    uint32_t maxBytesInFlight() const { return maxBytesInFlight_.load(std::memory_order_relaxed); }

    // Records waiting for the consumer right now. Exact when read from
    // either of the two threads; a snapshot from anywhere else. readSeq_
    // is loaded first on purpose: both sequence numbers only ever grow and
    // readSeq_ never passes writeSeq_, so reading the slower-moving one
    // first keeps the subtraction from underflowing for a third-party
    // observer. Reading them the other way round can.
    uint32_t recordsInFlight() const {
        const uint32_t readSeq = readSeq_.load(std::memory_order_acquire);
        return writeSeq_.load(std::memory_order_acquire) - readSeq;
    }

private:
    struct Slot {
        uint32_t offset = 0;
        uint32_t length = 0;
        // Arena bytes skipped ahead of `offset` to keep this record
        // contiguous; freed with it, so the two sides' byte accounting
        // stays symmetric.
        uint32_t padding = 0;
        uint32_t wireSize = 0;
        uint16_t command = 0;
    };

    static void recordHighWater(std::atomic<uint32_t>& highWater, uint32_t value) {
        // Single writer (the producer) - a plain load-then-store is enough,
        // same reasoning as QcSession::recordReassemblyOverflow()'s own
        // max tracking.
        if (value > highWater.load(std::memory_order_relaxed)) {
            highWater.store(value, std::memory_order_relaxed);
        }
    }

    std::array<uint8_t, ArenaCapacity> arena_{};
    std::array<Slot, MaxRecords> slots_{};

    // Producer-owned.
    uint32_t writeOffset_ = 0;
    std::atomic<uint32_t> writeSeq_{0};
    std::atomic<uint32_t> bytesClaimed_{0};
    std::atomic<uint32_t> droppedCount_{0};
    std::atomic<uint32_t> oversizedCount_{0};
    std::atomic<uint32_t> maxRecordsInFlight_{0};
    std::atomic<uint32_t> maxBytesInFlight_{0};

    // Consumer-owned.
    std::atomic<uint32_t> readSeq_{0};
    std::atomic<uint32_t> bytesReleased_{0};
};

}  // namespace qcbridge
