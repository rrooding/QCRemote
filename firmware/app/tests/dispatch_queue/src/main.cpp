// Issue #94: the dispatch path dropped 1-7 USB messages on every preset
// change on the unit. Two halves are covered here.
//
//  * QcMessageQueue.hpp on its own: the bounded SPSC queue that replaced
//    QcDispatcher's single in-flight slot - ordering, arena wrap-around,
//    both drop paths, and the high-water marks that are the fix's own
//    sizing evidence. Plain C++, no threads.
//
//  * QcDispatcher.hpp end to end on a real Zephyr thread, driven by a
//    reproduction of the owner's hardware burst: a producer that submits a
//    whole preset-change burst without ever letting the consumer run in
//    between, which is exactly what Zephyr's cooperative `usbh` thread
//    (K_PRIO_COOP(9) = priority -7) does to a preemptible dispatch thread
//    on hardware. The assertion is zero drops and every message delivered
//    in order.
//
// The producer here runs at a *cooperative* priority on purpose. That is
// the whole mechanism: a cooperative thread is not preempted by a ready
// higher-priority preemptible one, so the dispatch thread provably cannot
// run until the producer blocks - the same scheduling fact that made a
// one-slot mailbox lose the burst on hardware. This test fails against the
// pre-#94 single-slot QcDispatcher and passes against the queue.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "QcDispatcher.hpp"
#include "QcMessageQueue.hpp"
#include "QcMessageSink.hpp"
#include "QcMessageTally.hpp"
#include "QcStateReads.hpp"

using namespace qcbridge;

ZTEST_SUITE(dispatch_queue, NULL, NULL, NULL, NULL, NULL);

// --- QcMessageQueue.hpp ---------------------------------------------------

namespace {

// Small enough to exercise wrap-around and both full conditions in a test,
// unlike the firmware's own 128 KiB arena.
using TinyQueue = QcMessageQueue<64, 4>;

// Static, not a std::vector: this is also called from a thread with a
// 2048-byte stack, and the firmware's own no-heap rule
// (CODING_STANDARDS.md) is worth keeping in a test that exists to model
// the firmware's threading.
std::array<uint8_t, 256> pushScratch{};

bool pushBytes(TinyQueue& queue, uint16_t command, size_t length, uint8_t fill, size_t wireSize = 0) {
    zassert_true(length <= pushScratch.size(), "test payload too large for the scratch buffer");
    std::fill_n(pushScratch.begin(), length, fill);
    return queue.push(command, {pushScratch.data(), length}, wireSize == 0 ? length + 8 : wireSize);
}

}  // namespace

ZTEST(dispatch_queue, test_queue_starts_empty)
{
    TinyQueue queue;
    zassert_false(queue.front().has_value(), "a fresh queue must be empty");
    zassert_equal(queue.recordsInFlight(), 0u, "recordsInFlight");
    zassert_equal(queue.droppedCount(), 0u, "droppedCount");
    zassert_equal(queue.maxRecordsInFlight(), 0u, "maxRecordsInFlight");
    zassert_equal(queue.maxBytesInFlight(), 0u, "maxBytesInFlight");

    // pop() on an empty queue is a no-op, not a corruption of the read
    // index - the consumer's drain loop relies on that.
    queue.pop();
    zassert_equal(queue.recordsInFlight(), 0u, "recordsInFlight after a pop on empty");
}

ZTEST(dispatch_queue, test_queue_round_trips_a_record)
{
    TinyQueue queue;
    zassert_true(pushBytes(queue, 15, 5, 0xAB, 137), "push");

    const auto record = queue.front();
    zassert_true(record.has_value(), "front");
    zassert_equal(record->command, 15u, "command");
    zassert_equal(record->wireSize, 137u, "wireSize");
    zassert_equal(record->protoBytes.size(), 5u, "payload size");
    for (const uint8_t byte : record->protoBytes) {
        zassert_equal(byte, 0xAB, "payload byte");
    }

    queue.pop();
    zassert_false(queue.front().has_value(), "expected empty after pop");
}

ZTEST(dispatch_queue, test_queue_preserves_order_across_commands)
{
    // The real preset-change burst's command ids, in the order the device
    // sends them (RecallPreset itself arrives gzipped and goes to
    // QcGzipInflater, not here).
    const std::array<uint16_t, 5> burst = {kCommandGrid, kCommandSetlistPosition, kCommandScene,
                                           kCommandRecentsFavorites, kCommandPresetDirty};
    TinyQueue queue;
    for (size_t i = 0; i < burst.size(); ++i) {
        zassert_true(pushBytes(queue, burst[i], 3, static_cast<uint8_t>(i)), "push %zu", i);
        // Depth is 4, so drain as we go - order is what's under test here,
        // not capacity.
        if (i >= 2) {
            const auto record = queue.front();
            zassert_true(record.has_value(), "front %zu", i);
            queue.pop();
        }
    }
    zassert_equal(queue.droppedCount(), 0u, "nothing should have been dropped");

    for (size_t i = 3; i < burst.size(); ++i) {
        const auto record = queue.front();
        zassert_true(record.has_value(), "remaining front %zu", i);
        zassert_equal(record->command, burst[i], "command order at %zu", i);
        queue.pop();
    }
    zassert_false(queue.front().has_value(), "expected fully drained");
}

ZTEST(dispatch_queue, test_queue_drops_when_record_ring_is_full_not_the_arena)
{
    TinyQueue queue;  // 4 records over a 64-byte arena
    for (size_t i = 0; i < TinyQueue::kMaxRecords; ++i) {
        zassert_true(pushBytes(queue, 1, 1, static_cast<uint8_t>(i)), "push %zu", i);
    }
    // 4 bytes of a 64-byte arena used, but every descriptor slot is taken.
    zassert_false(pushBytes(queue, 1, 1, 0xFF), "a fifth record must be refused");
    zassert_equal(queue.droppedCount(), 1u, "droppedCount");
    zassert_equal(queue.oversizedCount(), 0u, "a full ring is not an oversized message");
    zassert_equal(queue.maxRecordsInFlight(), TinyQueue::kMaxRecords, "maxRecordsInFlight");

    // Draining one makes room again - the drop is transient by design.
    queue.pop();
    zassert_true(pushBytes(queue, 1, 1, 0xFE), "push after a pop");
    zassert_equal(queue.droppedCount(), 1u, "droppedCount must not grow on a successful push");
}

ZTEST(dispatch_queue, test_queue_drops_when_arena_is_full_not_the_record_ring)
{
    TinyQueue queue;
    zassert_true(pushBytes(queue, 4, 40, 0x11), "first large record");
    zassert_true(pushBytes(queue, 4, 20, 0x22), "second large record");
    // 60 of 64 bytes in flight, 2 of 4 descriptor slots used.
    zassert_false(pushBytes(queue, 4, 8, 0x33), "a record that doesn't fit the arena must be refused");
    zassert_equal(queue.droppedCount(), 1u, "droppedCount");
    zassert_equal(queue.oversizedCount(), 0u, "a transiently full arena is not an oversized message");
    zassert_equal(queue.maxBytesInFlight(), 60u, "maxBytesInFlight");

    queue.pop();  // frees 40
    zassert_true(pushBytes(queue, 4, 8, 0x33), "push after freeing the first record");
}

ZTEST(dispatch_queue, test_queue_counts_an_oversized_message_separately)
{
    // Bigger than the whole arena: no depth and no draining can ever help,
    // which is why QcDispatcher reports this apart from a full queue (it
    // means issue #66's streaming decoder, not a bigger buffer).
    TinyQueue queue;
    zassert_false(pushBytes(queue, 4, TinyQueue::kArenaCapacity + 1, 0x77), "oversized push");
    zassert_equal(queue.droppedCount(), 1u, "droppedCount counts it too");
    zassert_equal(queue.oversizedCount(), 1u, "oversizedCount");
    zassert_equal(queue.recordsInFlight(), 0u, "an oversized push must not consume a slot");

    // An exactly-arena-sized record is still accepted - the boundary is
    // "larger than", not "as large as".
    zassert_true(pushBytes(queue, 4, TinyQueue::kArenaCapacity, 0x88), "arena-sized push");
    zassert_equal(queue.maxBytesInFlight(), TinyQueue::kArenaCapacity, "maxBytesInFlight");
}

ZTEST(dispatch_queue, test_queue_wraps_the_arena_without_splitting_a_record)
{
    // Keeps two records in flight throughout, so the write offset really
    // does walk off the end of the arena and wrap (an empty queue rewinds
    // to offset 0 instead - see push()'s own comment). Every record must
    // come back contiguous and intact, and the tail skipped on each wrap
    // must be freed along with the record it was charged to - a leak there
    // would strand a few bytes per wrap and show up below as drops.
    TinyQueue queue;
    constexpr size_t kLength = 15;
    constexpr int kRounds = 40;

    zassert_true(pushBytes(queue, 1, kLength, 0x00), "priming push");
    for (int round = 0; round < kRounds; ++round) {
        const auto fill = static_cast<uint8_t>(0x40 + round);
        zassert_true(pushBytes(queue, 1, kLength, fill), "push round %d", round);

        // FIFO: this is the record pushed one iteration ago, so the one
        // just pushed stays in flight and holds the write offset where it
        // is.
        const auto record = queue.front();
        zassert_true(record.has_value(), "front round %d", round);
        zassert_equal(record->protoBytes.size(), kLength, "payload size round %d", round);
        const auto expected = round == 0 ? 0x00 : static_cast<uint8_t>(0x40 + round - 1);
        for (const uint8_t byte : record->protoBytes) {
            zassert_equal(byte, expected, "payload byte round %d - a split or overwritten record",
                          round);
        }
        queue.pop();
    }
    zassert_equal(queue.droppedCount(), 0u,
                  "%d wraps must not leak arena bytes - a leak shows up as drops here", kRounds);
    zassert_equal(queue.recordsInFlight(), 1u, "the last pushed record is still in flight");
}

ZTEST(dispatch_queue, test_queue_wrap_padding_is_charged_and_released)
{
    TinyQueue queue;
    zassert_true(pushBytes(queue, 1, 30, 0x01), "first record at offset 0");
    zassert_true(pushBytes(queue, 2, 20, 0x02), "second record at offset 30");
    queue.pop();  // retires the first, leaving the second in flight at 30

    // Write offset is at 50 of 64, and the queue isn't empty, so there's no
    // rewind: a 20-byte record can't fit the 14-byte tail, lands at offset
    // 0, and is charged 14 bytes of padding on top of its own 20.
    zassert_true(pushBytes(queue, 3, 20, 0x03), "wrapping push");

    const auto second = queue.front();
    zassert_true(second.has_value(), "front (second record)");
    zassert_equal(second->command, 2u, "command");
    for (const uint8_t byte : second->protoBytes) {
        zassert_equal(byte, 0x02, "the wrapping push must not have overwritten this record");
    }
    queue.pop();

    const auto third = queue.front();
    zassert_true(third.has_value(), "front (third record)");
    zassert_equal(third->command, 3u, "command");
    zassert_equal(third->protoBytes.size(), 20u, "payload size");
    for (const uint8_t byte : third->protoBytes) {
        zassert_equal(byte, 0x03, "payload byte - a split record would show stale 0x01 bytes");
    }
    queue.pop();

    // The padding came back with it, and the now-empty queue rewinds, so a
    // record needing the entire arena fits. Both halves matter: a leaked 14
    // bytes or a missing rewind each refuse this push.
    zassert_true(pushBytes(queue, 4, TinyQueue::kArenaCapacity, 0x04),
                 "a full-arena record must fit once the wrap padding is released");
    zassert_equal(queue.droppedCount(), 0u, "droppedCount");
    zassert_equal(queue.maxBytesInFlight(), TinyQueue::kArenaCapacity, "maxBytesInFlight");
}

ZTEST(dispatch_queue, test_queue_takes_a_full_arena_record_after_any_history)
{
    // ADR 0007's worst case is a `File` folder message exactly as large as
    // the arena, and the single slot this queue replaced always accepted
    // one. Walk the write offset to an awkward place first, drain, and the
    // next full-size message must still fit.
    TinyQueue queue;
    for (int i = 0; i < 3; ++i) {
        zassert_true(pushBytes(queue, 1, 7, static_cast<uint8_t>(i)), "warm-up push %d", i);
        queue.pop();
    }
    zassert_true(pushBytes(queue, 4, TinyQueue::kArenaCapacity, 0x55), "full-arena push");

    const auto record = queue.front();
    zassert_true(record.has_value(), "front");
    zassert_equal(record->protoBytes.size(), TinyQueue::kArenaCapacity, "payload size");
    for (const uint8_t byte : record->protoBytes) {
        zassert_equal(byte, 0x55, "payload byte");
    }
    zassert_equal(queue.droppedCount(), 0u, "droppedCount");
}

ZTEST(dispatch_queue, test_queue_accepts_a_zero_length_payload)
{
    // A bare message with no protobuf fields is legal on the wire; it must
    // not confuse the arena bookkeeping.
    TinyQueue queue;
    zassert_true(queue.push(kCommandKeepAlive, {}, 8), "zero-length push");
    const auto record = queue.front();
    zassert_true(record.has_value(), "front");
    zassert_equal(record->command, kCommandKeepAlive, "command");
    zassert_equal(record->protoBytes.size(), 0u, "payload size");
    queue.pop();
    zassert_equal(queue.droppedCount(), 0u, "droppedCount");
}

// --- QcDispatcher.hpp: the preset-change burst, on real threads -----------

namespace {

// The owner's hardware burst (issue #94), minus RecallPreset itself: the
// device sends a gzipped RecallPreset (handled by QcGzipInflater) and then
// these, back to back, on every preset change.
constexpr std::array<uint16_t, 5> kPresetChangeBurst = {kCommandGrid, kCommandSetlistPosition,
                                                        kCommandScene, kCommandRecentsFavorites,
                                                        kCommandPresetDirty};

// Representative payload size for one of those messages. Deliberately not
// tiny: the point is that a whole burst's worth fits the arena at once.
constexpr size_t kBurstPayloadSize = 256;

constexpr int kPresetChangeRounds = 24;  // > the 20 the issue's done-criteria asks for

class RecordingSink : public QcMessageSink {
public:
    void onMessage(uint16_t command, std::span<const uint8_t> protoBytes, size_t wireSize,
                   size_t inflatedSize, bool compressed) override {
        (void)wireSize;
        (void)inflatedSize;
        (void)compressed;
        // Every byte must still be the sender's - a queue that let the
        // producer reuse a record's arena bytes before this call returned
        // would show up as a mismatch here, not as a drop.
        bool intact = protoBytes.size() == kBurstPayloadSize;
        for (const uint8_t byte : protoBytes) {
            intact = intact && byte == static_cast<uint8_t>(command);
        }
        if (!intact) {
            ++corruptCount_;
        }
        if (deliveredCount_ < delivered_.size()) {
            delivered_[deliveredCount_] = command;
        }
        ++deliveredCount_;
    }

    void onInflateFailed(uint16_t, size_t, const char*, std::optional<size_t>) override {}

    size_t deliveredCount() const { return deliveredCount_; }
    size_t corruptCount() const { return corruptCount_; }
    uint16_t deliveredCommand(size_t index) const { return delivered_[index]; }

private:
    std::array<uint16_t, kPresetChangeBurst.size() * kPresetChangeRounds> delivered_{};
    size_t deliveredCount_ = 0;
    size_t corruptCount_ = 0;
};

// Static: QcDispatcher carries the firmware's own 128 KiB arena, far too
// much for a Ztest thread stack.
RecordingSink sink;
QcDispatcher dispatcher;

// Mirrors Zephyr's `usbh` thread, which is what submit() is called from on
// hardware: cooperative, so a ready dispatch thread provably cannot run
// until this one blocks. K_PRIO_COOP(0) is the highest cooperative
// priority available, i.e. strictly above the dispatcher's own.
K_THREAD_STACK_DEFINE(producerStack, 2048);
struct k_thread producerThread;
K_SEM_DEFINE(producerDoneSem, 0, 1);

// Refilled per message, and reused immediately after submit() returns -
// which is the point: submit() must have copied the bytes into the queue's
// own arena before returning, exactly as `usbh` reuses the reassembly
// buffer for the next message.
std::array<uint8_t, kBurstPayloadSize> burstPayload{};

void producerEntry(void*, void*, void*)
{
    for (int round = 0; round < kPresetChangeRounds; ++round) {
        // One preset change: the whole burst submitted without a single
        // scheduling point in between, exactly as `usbh` delivers it while
        // an inflate or a cooperative Wi-Fi thread is keeping the consumer
        // off the CPU.
        for (const uint16_t command : kPresetChangeBurst) {
            std::fill(burstPayload.begin(), burstPayload.end(), static_cast<uint8_t>(command));
            dispatcher.submit(command, {burstPayload.data(), burstPayload.size()},
                              burstPayload.size() + 8);
        }
        // Between changes, let the consumer drain - a real preset change is
        // seconds apart from the next one.
        k_msleep(5);
    }
    k_sem_give(&producerDoneSem);
}

}  // namespace

ZTEST(dispatch_queue, test_dispatcher_delivers_a_preset_change_burst_with_no_drops)
{
    dispatcher.setSink(sink);
    dispatcher.start();

    k_thread_create(&producerThread, producerStack, K_THREAD_STACK_SIZEOF(producerStack),
                    &producerEntry, nullptr, nullptr, nullptr, K_PRIO_COOP(0), 0, K_NO_WAIT);
    k_thread_name_set(&producerThread, "test_usbh");

    zassert_equal(k_sem_take(&producerDoneSem, K_SECONDS(30)), 0, "producer did not finish");
    // The dispatcher thread drains on its own; give it room to finish the
    // last burst before reading the counters.
    k_msleep(200);

    constexpr size_t expected = kPresetChangeBurst.size() * kPresetChangeRounds;
    zassert_equal(dispatcher.droppedCount(), 0u,
                  "expected no dispatch drops across %d preset-change bursts, got %u",
                  kPresetChangeRounds, dispatcher.droppedCount());
    zassert_equal(dispatcher.oversizedCount(), 0u, "oversizedCount");
    zassert_equal(sink.deliveredCount(), expected, "expected every burst message to reach the sink");
    zassert_equal(sink.corruptCount(), 0u,
                  "a record's arena bytes were reused before the sink was done with them");

    // Order must survive the queue, per round and across rounds.
    for (size_t i = 0; i < expected; ++i) {
        zassert_equal(sink.deliveredCommand(i), kPresetChangeBurst[i % kPresetChangeBurst.size()],
                      "delivery order at index %zu", i);
    }

    // The sizing evidence the fix reports on hardware. A whole burst can
    // sit in the queue at once, so the peak must be at least one burst - a
    // single-slot dispatcher could never read above 1.
    zassert_true(dispatcher.maxRecordsInFlight() >= kPresetChangeBurst.size(),
                 "expected the queue to have actually held a whole burst, peak was %u",
                 dispatcher.maxRecordsInFlight());
    zassert_true(dispatcher.maxRecordsInFlight() <= QcDispatcher::queueDepth(),
                 "peak occupancy must stay within the configured depth");
    zassert_true(dispatcher.maxBytesInFlight() <= QcDispatcher::arenaCapacity(),
                 "peak arena use must stay within the arena");
}
