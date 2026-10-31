#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "QcGzip.hpp"

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
//      because of (1); a failed/successful inflate is worth logging
//      (there's no consumer for the decompressed bytes yet - see below -
//      so logging the outcome is the only observable result for now),
//      and LOG_* must not run on `usbh` either per CODING_STANDARDS.md.
//
// QcSession::handleMessage() detects the gzip magic prefix itself (cheap:
// a 3-byte compare, no heavy stack) and, if present, hands the compressed
// bytes to submit() below instead of inflating them inline.
//
// Single in-flight slot: a second compressed message arriving before the
// first has finished inflating is dropped (submit() returns false)
// rather than queued. Acceptable for now - there is no real consumer of
// decompressed payload yet. No command currently dispatched by
// handleMessage() is ever compressed in practice (Version/Connection/
// ResetCommsBuffers/KeepAlive are all small and uncompressed, confirmed
// in PR #56), and the one realistic candidate (ModelRepo) has no handler
// yet (issue #5 Tier 2 / #9). This class exists to implement the
// generic, command-id-agnostic detect+inflate step at its seam (issue
// #6) and exercise+log it against real traffic once compressed messages
// do start arriving, ahead of a real consumer existing to hand the
// decompressed bytes to. Revisit the single-slot limitation if/when one
// does, and back-to-back compressed messages turn out to matter.
class QcGzipInflater {
public:
    QcGzipInflater() { k_sem_init(&itemReadySem_, 0, 1); }

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
    bool submit(uint16_t command, std::span<const uint8_t> compressed) {
        if (busy_.exchange(true)) {
            return false;
        }
        if (compressed.size() > mailbox_.size()) {
            busy_.store(false);
            return false;
        }
        std::copy(compressed.begin(), compressed.end(), mailbox_.begin());
        mailboxLen_ = compressed.size();
        mailboxCommand_ = command;
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
            } else {
                // Named, not just the raw enum value, so the first real
                // compressed message that fails shows up in a log as
                // e.g. "CrcMismatch" rather than an opaque "error 5" -
                // see ADR 0005's note on qc-mcp's gunzip() fallback for
                // why a failure here needs to be obvious, not silent.
                LOG_WRN("Inflate failed for command %u (%zu compressed bytes): %s", mailboxCommand_, mailboxLen_,
                        gzip::toString(result.error()));
            }
            // No dispatch of `result`'s decompressed bytes yet - see this
            // class's header comment. A future consumer (#5 Tier 2 / #9)
            // reads them here, before busy_ is cleared.

            busy_.store(false);
        }
    }

    K_KERNEL_STACK_MEMBER(stack_, 4096);
    struct k_thread thread_ {};
    bool threadStarted_ = false;
    struct k_sem itemReadySem_ {};

    std::atomic<bool> busy_{false};
    std::array<uint8_t, gzip::kMaxCompressedInputSize> mailbox_{};
    size_t mailboxLen_ = 0;
    uint16_t mailboxCommand_ = 0;
    std::array<uint8_t, gzip::kMaxInflatedSize> output_{};
};

}  // namespace qcbridge
