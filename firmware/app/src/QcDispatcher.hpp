#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <zephyr/kernel.h>

#include "QcMessageQueue.hpp"
#include "QcMessageSink.hpp"

namespace qcbridge {

// Runs QcMessageSink::onMessage() on its own thread, off Zephyr's internal
// `usbh` thread - the uncompressed-message counterpart to
// QcGzipInflater.hpp, which does the same for messages that need an inflate
// step first. See QcSession::handleMessage() for why usbh thread work has
// to stay to a single bounded memcpy: no decoding beyond the message
// trailer, no logging, no real dispatch logic.
//
// Issue #94: this used to be a single in-flight slot, and the owner's
// hardware run found it losing 1-7 messages on *every* preset change on
// the unit (RecallPreset arrives gzipped and 14-23 KB, then Grid,
// SetlistPosition, Scene and RecentsFavorites follow as a burst). A single
// slot can't work here, for a structural reason rather than a tuning one:
// the producer is Zephyr's `usbh` thread at K_PRIO_COOP(9) = priority -7,
// cooperative, which the scheduler always picks ahead of this preemptible
// consumer. So whenever the consumer's turn is delayed - by a CPU-bound
// gzip::inflate() at the same priority, by a cooperative net/HTTP-server
// thread (K_PRIO_COOP(15) = -1) servicing Wi-Fi, by a synchronous
// CONFIG_LOG_MODE_IMMEDIATE log line - every further message in the burst
// found the slot busy and was dropped. QcMessageQueue.hpp replaces it with
// a bounded SPSC queue that absorbs the burst instead; see that file for
// the full mechanism and the memory trade-off.
//
// The matching scheduling change lives in QcGzipInflater.hpp, which now
// runs *below* this thread's priority so an inflate can't hold dispatch of
// the preset-identity messages behind it for milliseconds at a time. The
// queue bounds the loss; the priority ordering keeps the queue shallow and
// the dispatch latency low, which is what #15/#78 need.
//
// droppedCount() still exists and is still reported (QcFileStream.hpp's
// Summary, QcSession::checkTrafficHealth()) - the point was never that
// drops are impossible, only that they shouldn't happen under the traffic
// this device actually produces. maxRecordsInFlight()/maxBytesInFlight()
// are the companion evidence: they say how much of the queue a real burst
// uses, so the depth below stays a measured number rather than a guess.
class QcDispatcher {
public:
    QcDispatcher() { k_sem_init(&itemReadySem_, 0, 1); }

    void setSink(QcMessageSink& sink) { sink_ = &sink; }

    // Lifetime count of messages dropped by submit() below (queue full or
    // too big for the arena, combined - both are "this message never
    // reached the sink"). Monotonic, never reset: callers wanting a count
    // scoped to one stream (QcFileStream.hpp) snapshot this at the start
    // and diff at the end, same as QcSession does for
    // QcGzipInflater::droppedCount().
    uint32_t droppedCount() const { return queue_.droppedCount(); }

    // The subset of droppedCount() that no queue depth could have helped:
    // a single message larger than the whole arena (issue #66's 113 KB+
    // full-plugin-listing File messages are the realistic case).
    uint32_t oversizedCount() const { return queue_.oversizedCount(); }

    // Peak queue occupancy since boot - issue #94's burst measurement.
    uint32_t maxRecordsInFlight() const { return queue_.maxRecordsInFlight(); }
    uint32_t maxBytesInFlight() const { return queue_.maxBytesInFlight(); }
    static constexpr uint32_t queueDepth() { return static_cast<uint32_t>(kQueueDepth); }
    static constexpr uint32_t arenaCapacity() { return static_cast<uint32_t>(kArenaCapacity); }

    // Lazily starts the worker thread - same pattern as
    // QcGzipInflater::start()/QcSession's own threads. Idempotent.
    void start() {
        if (threadStarted_) {
            return;
        }
        k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_), &threadEntryTrampoline, this,
                        nullptr, nullptr, kThreadPriority, 0, K_NO_WAIT);
        k_thread_name_set(&thread_, "qc_dispatch");
        threadStarted_ = true;
    }

    // Called from QcSession::handleMessage() on the `usbh` thread - must
    // stay cheap: a single bounded memcpy, nothing else, plus an atomic
    // counter bump on the drop paths - no logging, same safety requirement
    // as the rest of that function. Returns false (message dropped) only
    // if the queue is full or `protoBytes` is larger than the whole arena.
    bool submit(uint16_t command, std::span<const uint8_t> protoBytes, size_t wireSize) {
        if (!queue_.push(command, protoBytes, wireSize)) {
            return false;
        }
        k_sem_give(&itemReadySem_);
        return true;
    }

private:
    // Above QcGzipInflater's priority (7) as of issue #94, not equal to it:
    // dispatch is a counter update plus at most a small decode, while an
    // inflate is milliseconds of CPU work on a 14-23 KB RecallPreset
    // payload. Equal priorities meant the inflater - which never blocks
    // mid-inflate - held every queued message behind it (Zephyr's default
    // CONFIG_TIMESLICE_SIZE is 20 ms, far longer than a whole inflate, so
    // time slicing never broke it up). Still below the heartbeat's 5: a
    // stalled dispatch must not delay a KeepAlive (issue #8).
    static constexpr int kThreadPriority = 6;

    // Issue #94's burst sizing. The owner's hardware log showed deltas of
    // up to 7 dropped messages per preset change, i.e. at least 8 messages
    // delivered inside one stretch where this thread didn't get a turn; a
    // preset change itself is 5 pushes (RecallPreset goes to
    // QcGzipInflater, so Grid/SetlistPosition/Scene/RecentsFavorites/
    // PresetDirty land here) on top of GlobalTempo's continuous ~4/s
    // stream. 32 is roughly 4x the worst burst observed, and costs 32
    // descriptors (20 bytes each, ~640 B) because the payloads share one
    // arena rather than each slot carrying its own worst-case buffer.
    static constexpr size_t kQueueDepth = 32;

    // Mirrors QcHidBridge.hpp's kReassemblyCapacity - a message's protobuf
    // payload can never be bigger than the whole reassembled HID message it
    // came from. Not shared as one constant for the same reason QcGzip.hpp's
    // comment gives about kMaxCompressedInputSize: including QcHidBridge.hpp
    // back here would be circular. Keep in sync by hand - issue #33's final
    // sizing (ADR 0007) is 131072, matching kReassemblyCapacity: uncompressed
    // `File` messages (confirmed on every hardware run so far: 0 compressed)
    // go through this arena, not QcGzipInflater's. Follows the same
    // CONFIG_QC_MESSAGE_CAPACITY override as kReassemblyCapacity.
    //
    // Deliberately unchanged by issue #94: the whole burst that was being
    // dropped is small messages (tens to hundreds of bytes each), and a
    // File listing and a preset change never overlap in practice, so the
    // existing 128 KiB buys the queue depth for free. A single File
    // message that really does fill the arena still blocks the next one -
    // exactly as the old single slot did - and the fix for that case is
    // issue #66's streaming decoder, not a bigger buffer. oversizedCount()
    // and maxBytesInFlight() are what would show it happening.
#if defined(CONFIG_QC_MESSAGE_CAPACITY)
    static constexpr size_t kArenaCapacity = CONFIG_QC_MESSAGE_CAPACITY;
#else
    static constexpr size_t kArenaCapacity = 131072;
#endif

    static void threadEntryTrampoline(void* p1, void*, void*) { static_cast<QcDispatcher*>(p1)->run(); }

    void run() {
        while (true) {
            k_sem_take(&itemReadySem_, K_FOREVER);
            // Drains everything queued, not one record per wake-up: the
            // semaphore's limit is 1, so a burst of pushes collapses into a
            // single signal and the leftovers would otherwise sit there
            // until the next message arrived.
            while (const auto record = queue_.front()) {
                if (sink_ != nullptr) {
                    sink_->onMessage(record->command, record->protoBytes, record->wireSize,
                                     record->protoBytes.size(), false);
                }
                // After the sink call, never before: the sink reads
                // `protoBytes` straight out of the arena (QcSession's
                // onMessage() decodes File presets from it), and pop() is
                // what lets the producer reuse those bytes.
                queue_.pop();
            }
        }
    }

    K_KERNEL_STACK_MEMBER(stack_, 2048);
    struct k_thread thread_ {};
    bool threadStarted_ = false;
    struct k_sem itemReadySem_ {};

    QcMessageSink* sink_ = nullptr;
    QcMessageQueue<kArenaCapacity, kQueueDepth> queue_;
};

}  // namespace qcbridge
