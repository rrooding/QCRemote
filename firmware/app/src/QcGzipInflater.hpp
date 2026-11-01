#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "QcGzip.hpp"
#include "QcMessageSink.hpp"

namespace qcbridge {

// Runs gzip::inflate() on its own dedicated thread, off Zephyr's internal
// `usbh` thread that calls QcHidBridge::onReportIn()/QcSession::handleMessage().
// That thread's stack (CONFIG_USBH_STACK_SIZE, 1536 bytes) already
// overflowed once this session from a single LOG_INF call added deep in
// handleMessage() - see QcSession.hpp's comment on that function. Inflate
// is a worse fit there than that log call was, for three separate
// reasons, any one of which would be enough on its own:
//   1. tinfl's own working state is a ~8KB struct (TINFL_MAX_HUFF_TABLES
//      lookup tables) - QcGzip.hpp keeps that out of any call stack via a
//      function-local static, but the surrounding call frames (gzip
//      header parsing, CRC32, this class's own trampolines) still have a
//      real, non-zero stack cost that a 1536-byte stack has little
//      margin for (THREAD_ANALYZER measured usbh's peak usage at 736
//      bytes with *no* extra work added).
//   2. Decompression is actual CPU work, not a fixed-cost memcpy. Running
//      it on the same thread that services the USB host controller's own
//      report timing risks delaying that controller's housekeeping for
//      however long inflate takes, independent of stack safety.
//   3. QcSession::handleMessage() is documented log-free specifically
//      because of (1); a failed/successful inflate is worth logging, and
//      LOG_* must not run on `usbh` either per CODING_STANDARDS.md.
//
// QcSession::handleMessage() detects the gzip magic prefix itself (cheap:
// a 3-byte compare, no heavy stack) and, if present, hands the compressed
// bytes to submit() below instead of inflating them inline.
//
// Single in-flight slot: a second compressed message arriving before the
// first has finished inflating is dropped (submit() returns false)
// rather than queued. No command currently dispatched by handleMessage()
// is commonly compressed in practice (Version/Connection/
// ResetCommsBuffers/KeepAlive are all small and uncompressed, confirmed
// in PR #56) except File (issue #60), whose folder-listing payloads this
// class exists to exercise against real traffic. Revisit the single-slot
// limitation if/when back-to-back compressed messages turn out to matter
// for File's ~400-message stream - QcFileStream.hpp's summary counts
// failures rather than assuming every message got through.
//
// On success, the decompressed bytes go to QcSession (via QcMessageSink -
// see setSink()) from this class's own worker thread, same as
// QcDispatcher.hpp does for uncompressed messages. On failure, there's no
// usable protoBytes to hand over, so onInflateFailed() goes to the sink
// instead - still worth counting for a command like File mid-stream.
//
// Same open question as QcDispatcher.hpp on whether one slot is enough:
// inflate is real CPU work (unlike a plain memcpy), so it's the more
// plausible of the two to actually fall behind the File stream's arrival
// rate - but there's still no hardware measurement of either side of that
// comparison. droppedCount() is how that gets answered empirically.
class QcGzipInflater {
public:
    QcGzipInflater() { k_sem_init(&itemReadySem_, 0, 1); }

    void setSink(QcMessageSink& sink) { sink_ = &sink; }

    // Lifetime count of messages dropped by submit() below (busy slot or
    // too big for the mailbox, combined). Monotonic, never reset - see
    // QcDispatcher::droppedCount()'s comment for why callers scoped to one
    // stream need to snapshot-and-diff.
    uint32_t droppedCount() const { return droppedCount_.load(std::memory_order_relaxed); }

    // Lazily starts the worker thread. Called from QcSession::deviceConnected()
    // (usbh thread) alongside its own lazy thread start, for the same
    // reason: avoid creating kernel threads before the kernel is known to
    // be fully up. Idempotent.
    void start() {
        if (threadStarted_) {
            return;
        }
        k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_), &threadEntryTrampoline, this, nullptr,
                        nullptr, kThreadPriority, 0, K_NO_WAIT);
        k_thread_name_set(&thread_, "qc_gzip_inflate");
        threadStarted_ = true;
    }

    // Called from QcSession::handleMessage() on the `usbh` thread - must
    // stay cheap (a single bounded memcpy, no logging, no inflate; see
    // this class's header comment for why). Returns false if a previous
    // item hasn't finished processing yet, or if `compressed` is larger
    // than this module supports (gzip::kMaxCompressedInputSize) - either
    // way the caller just drops the message, same as it already does for
    // HidReassembler::Status::Overflow/Malformed.
    bool submit(uint16_t command, std::span<const uint8_t> compressed, size_t wireSize) {
        if (busy_.exchange(true)) {
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (compressed.size() > mailbox_.size()) {
            busy_.store(false);
            droppedCount_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::copy(compressed.begin(), compressed.end(), mailbox_.begin());
        mailboxLen_ = compressed.size();
        mailboxCommand_ = command;
        mailboxWireSize_ = wireSize;
        k_sem_give(&itemReadySem_);
        return true;
    }

private:
    // Between the handshake thread's priority (7) and the heartbeat
    // thread's (5, see QcSession.hpp) - a stalled inflate shouldn't be
    // able to starve the KeepAlive heartbeat, but it's also not as
    // latency-critical as the heartbeat is.
    static constexpr int kThreadPriority = 6;

    static void threadEntryTrampoline(void* p1, void*, void*) { static_cast<QcGzipInflater*>(p1)->run(); }

    void run() {
        while (true) {
            k_sem_take(&itemReadySem_, K_FOREVER);

            const auto result =
                gzip::inflate({mailbox_.data(), mailboxLen_}, {output_.data(), output_.size()});
            if (result) {
                LOG_INF("Inflated command %u: %zu -> %zu bytes", mailboxCommand_, mailboxLen_, result->size());
                if (sink_ != nullptr) {
                    sink_->onMessage(mailboxCommand_, *result, mailboxWireSize_, result->size(), true);
                }
            } else {
                // issue #33 / ADR 0007: the gzip trailer's ISIZE field can
                // give the true decompressed size without finishing
                // decompression - but only when inflate() got far enough to
                // know the bytes at that fixed offset really are a trailer.
                // That's true for OutputTooLarge only (a real deflate stream
                // decoded far enough to overflow `output`); every other
                // InflateError (Truncated, HeaderMalformed, DecompressFailed,
                // CrcMismatch, NotGzipped) means the input past the header
                // either isn't a real gzip member or never finished, so
                // whatever sits at that offset is not a trustworthy size -
                // see QcGzip.hpp's peekDecompressedSize() comment (fixed by
                // a Copilot PR review: this used to call peekDecompressedSize()
                // unconditionally, which reported a huge false size off the
                // 15-byte kTruncatedGzip fixture). Leave the size unknown for
                // every other error rather than guess.
                const bool sizeIsTrustworthy = result.error() == gzip::InflateError::OutputTooLarge;
                const auto advertisedInflatedSize =
                    sizeIsTrustworthy ? gzip::peekDecompressedSize({mailbox_.data(), mailboxLen_})
                                       : std::nullopt;

                // Named, not just the raw enum value, so the first real
                // compressed message that fails shows up in a log as
                // e.g. "CrcMismatch" rather than an opaque "error 5" -
                // see ADR 0005's note on qc-mcp's gunzip() fallback for
                // why a failure here needs to be obvious, not silent.
                // "advertised": this is the gzip trailer's own claim, read
                // without decompressing - not a size this module verified.
                if (advertisedInflatedSize) {
                    LOG_WRN("Inflate failed for command %u (%zu compressed bytes, advertised inflated "
                            "size %u): %s",
                            mailboxCommand_, mailboxLen_, *advertisedInflatedSize,
                            gzip::toString(result.error()));
                } else {
                    LOG_WRN("Inflate failed for command %u (%zu compressed bytes): %s", mailboxCommand_,
                            mailboxLen_, gzip::toString(result.error()));
                }
                if (sink_ != nullptr) {
                    sink_->onInflateFailed(mailboxCommand_, mailboxWireSize_, gzip::toString(result.error()),
                                           advertisedInflatedSize);
                }
            }

            busy_.store(false);
        }
    }

    K_KERNEL_STACK_MEMBER(stack_, 4096);
    struct k_thread thread_ {};
    bool threadStarted_ = false;
    struct k_sem itemReadySem_ {};

    QcMessageSink* sink_ = nullptr;
    std::atomic<bool> busy_{false};
    std::atomic<uint32_t> droppedCount_{0};
    std::array<uint8_t, gzip::kMaxCompressedInputSize> mailbox_{};
    size_t mailboxLen_ = 0;
    size_t mailboxWireSize_ = 0;
    uint16_t mailboxCommand_ = 0;
    std::array<uint8_t, gzip::kMaxInflatedSize> output_{};
};

}  // namespace qcbridge
