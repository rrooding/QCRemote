#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <zephyr/kernel.h>

#include "QcMessageSink.hpp"

namespace qcbridge {

// Runs QcMessageSink::onMessage() on its own thread, off Zephyr's internal
// `usbh` thread - the uncompressed-message counterpart to
// QcGzipInflater.hpp, which does the same for messages that need an inflate
// step first. See QcSession::handleMessage() for why usbh thread work has
// to stay to a single bounded memcpy: no decoding beyond the message
// trailer, no logging, no real dispatch logic.
//
// Single in-flight slot, same tradeoff QcGzipInflater.hpp already made: a
// message arriving while the previous one is still being dispatched is
// dropped rather than queued. Dispatch work today is cheap (a counter
// update, an occasional log line), so this should only drop under
// pathological bursts - not something to assume can't happen, though:
// QcFileStream.hpp's summary reports counts rather than assuming every
// message was seen.
//
// Whether a single slot is actually enough against the File stream's
// ~400-messages-over-~12s arrival rate isn't something this codebase can
// determine without hardware: dispatch work here is "cheap" by
// inspection (a sink callback that does a counter update and maybe one
// log line), but there's no measured number for how long that actually
// takes on the real target versus how bursty the device's own send
// timing is. droppedCount() exists precisely to answer this
// empirically rather than by guessing: if it reads 0 across a real run,
// one slot keeps up; if it's consistently above 0, that's real evidence
// of data loss by design, and the fix is a small bounded queue - not
// something to add speculatively ahead of that evidence (see
// QcFileStream.hpp's Summary and firmware/README.md's issue #60 section
// for how this gets surfaced).
class QcDispatcher {
public:
    QcDispatcher() { k_sem_init(&itemReadySem_, 0, 1); }

    void setSink(QcMessageSink& sink) { sink_ = &sink; }

    // Lifetime count of messages dropped by submit() below (busy slot or
    // too big for the mailbox, combined - both are "this message never
    // reached the sink"). Monotonic, never reset: callers wanting a count
    // scoped to one stream (QcFileStream.hpp) snapshot this at the start
    // and diff at the end, same as QcSession does for
    // QcGzipInflater::droppedCount().
    uint32_t droppedCount() const { return droppedCount_.load(std::memory_order_relaxed); }

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
    // counter bump on the drop paths (droppedCount_) - no logging, same
    // safety requirement as the rest of this function. Returns false
    // (message dropped) if a previous item hasn't finished dispatching yet,
    // or if `protoBytes` is larger than this module's mailbox.
    bool submit(uint16_t command, std::span<const uint8_t> protoBytes, size_t wireSize) {
        if (busy_.exchange(true)) {
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (protoBytes.size() > mailbox_.size()) {
            busy_.store(false);
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::copy(protoBytes.begin(), protoBytes.end(), mailbox_.begin());
        mailboxLen_ = protoBytes.size();
        mailboxCommand_ = command;
        mailboxWireSize_ = wireSize;
        k_sem_give(&itemReadySem_);
        return true;
    }

private:
    // Same priority band as QcGzipInflater - this isn't more or less urgent
    // than an inflate.
    static constexpr int kThreadPriority = 6;

    // Mirrors QcHidBridge.hpp's kReassemblyCapacity - a message's protobuf
    // payload can never be bigger than the whole reassembled HID message it
    // came from. Not shared as one constant for the same reason QcGzip.hpp's
    // comment gives about kMaxCompressedInputSize: including QcHidBridge.hpp
    // back here would be circular. Keep in sync by hand - issue #33's final
    // sizing (ADR 0007) is 131072, matching kReassemblyCapacity: uncompressed
    // `File` messages (confirmed on every hardware run so far: 0 compressed)
    // go through this mailbox, not QcGzipInflater's.
    static constexpr size_t kMailboxCapacity = 131072;

    static void threadEntryTrampoline(void* p1, void*, void*) { static_cast<QcDispatcher*>(p1)->run(); }

    void run() {
        while (true) {
            k_sem_take(&itemReadySem_, K_FOREVER);
            if (sink_ != nullptr) {
                sink_->onMessage(mailboxCommand_, {mailbox_.data(), mailboxLen_}, mailboxWireSize_,
                                 mailboxLen_, false);
            }
            busy_.store(false);
        }
    }

    K_KERNEL_STACK_MEMBER(stack_, 2048);
    struct k_thread thread_ {};
    bool threadStarted_ = false;
    struct k_sem itemReadySem_ {};

    QcMessageSink* sink_ = nullptr;
    std::atomic<bool> busy_{false};
    std::atomic<uint32_t> droppedCount_{0};
    std::array<uint8_t, kMailboxCapacity> mailbox_{};
    size_t mailboxLen_ = 0;
    size_t mailboxWireSize_ = 0;
    uint16_t mailboxCommand_ = 0;
};

}  // namespace qcbridge
