#pragma once

#include <atomic>
#include <cstdint>
#include <cerrno>
#include <optional>
#include <span>
#include <string_view>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <etl/vector.h>

#include "HidChunker.hpp"
#include "HidReassembler.hpp"
#include "QcDispatcher.hpp"
#include "QcFileRequest.hpp"
#include "QcFileStream.hpp"
#include "QcGzip.hpp"
#include "QcGzipInflater.hpp"
#include "QcHandshakeMessages.hpp"
#include "QcMessage.hpp"
#include "QcMessageSink.hpp"
#include "ScopedKMutex.hpp"
#include "UsbHostClass.hpp"

namespace qcbridge {

// Runs the connect-time handshake (issue #7: ResetCommsBuffers, Version,
// Connection - see PROTOCOL.md section 3, steps 1-3) and the ~200ms
// KeepAlive heartbeat (issue #8) once a device is claimed.
//
// Both need their own thread. qc_usbh_send_report() is a *blocking*
// control transfer (up to Zephyr's 5s setup-request timeout) that only
// completes once the USB host stack's own thread finishes processing it -
// and that's the same thread that calls onProbe()/onReportIn(). Sending
// from either of those callbacks directly would deadlock the host stack
// against itself. So QcHidBridge only ever *signals* this class (via
// semaphores, never blocking); the actual sends happen on dedicated
// threads instead. The heartbeat thread is started *before* the handshake
// sends anything (see runHandshake()), matching qc-mcp's own
// transport.py::open() order - its comment on this is explicit: the
// heartbeat is what "keep[s] session online (required for reads)".
// sendMessage() is shared by both threads and guarded by sendMutex_ so
// their HID reports can never interleave on the wire.
//
// General message dispatch (issue #60): handleMessage() itself stays
// minimal (see its own comment below) and hands every reassembled message
// to either gzipInflater_ or dispatcher_, each of which calls back into this
// class's QcMessageSink overrides (onMessage()/onInflateFailed()) from its
// own worker thread - never from the `usbh` thread. That's also where the
// File-read stream (fileStream_, see QcFileStream.hpp) gets its per-message
// hook, kicked off right after the handshake in runHandshake() and polled
// for its quiet/overall timeout from the heartbeat thread in runHeartbeat().
class QcSession : public QcMessageSink {
public:
    QcSession() {
        k_sem_init(&deviceReadySem_, 0, 1);
        k_sem_init(&versionReplySem_, 0, 1);
        k_sem_init(&heartbeatSem_, 0, 1);
        k_mutex_init(&sendMutex_);
        k_mutex_init(&fileStreamMutex_);
        k_timer_init(&heartbeatTimer_, &heartbeatTimerExpiryTrampoline, nullptr);
        k_timer_user_data_set(&heartbeatTimer_, this);
        dispatcher_.setSink(*this);
        gzipInflater_.setSink(*this);
    }

    // Called from onProbe() (host stack thread) once a device is accepted.
    // Never blocks: starts the handshake thread on first use, then just
    // wakes it.
    void deviceConnected() {
        if (!threadStarted_) {
            k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_), &threadEntryTrampoline,
                            this, nullptr, nullptr, kThreadPriority, 0, K_NO_WAIT);
            k_thread_name_set(&thread_, "qc_session");
            threadStarted_ = true;
        }
        dispatcher_.start();
        gzipInflater_.start();
        k_sem_give(&deviceReadySem_);
    }

    // Called from onReportIn() (host stack thread) once HidReassembler has
    // completed a message. Hands the message off to a thread that can
    // safely do real dispatch work and log (issue #60) - never processes it
    // here.
    //
    // Deliberately does no logging of its own, and does not inflate a
    // gzip-compressed protoBytes in place - only detects it (issue #6: any
    // command's protobuf may be gzip'd, generically, not just ModelRepo's).
    // A diagnostic LOG_INF added here during issue #8's hardware debugging
    // crashed real hardware with a stack overflow: this runs on Zephyr's
    // own internal `usbh` thread (CONFIG_USBH_STACK_SIZE), not one of ours,
    // and CONFIG_LOG_MODE_IMMEDIATE means a log call's cbprintf formatting
    // runs synchronously on the calling thread's own stack. Keep this
    // function free of logging and of real work (decoding beyond the
    // message trailer, dispatch logic, and - see QcGzipInflater.hpp for why
    // that includes gzip::inflate() - decompression) rather than
    // re-deriving that the hard way again.
    void handleMessage(std::span<const uint8_t> message) {
        const auto decoded = decodeMessage(message);
        if (!decoded) {
            return;
        }

        if (gzip::hasMagic(decoded->protoBytes)) {
            gzipInflater_.submit(decoded->command, decoded->protoBytes, message.size());
            return;
        }

        dispatcher_.submit(decoded->command, decoded->protoBytes, message.size());
    }

    // QcMessageSink overrides - called from dispatcher_'s or gzipInflater_'s
    // own worker thread (QcDispatcher.hpp/QcGzipInflater.hpp), never from
    // `usbh`. Safe to log and do real work here.
    void onMessage(uint16_t command, std::span<const uint8_t> protoBytes, size_t wireSize,
                   size_t inflatedSize, bool compressed) override {
        if (command == kCommandVersion && waitingForVersionReply_.load()) {
            pendingVersionReply_ = decodeVersionReply(protoBytes);
            k_sem_give(&versionReplySem_);
        }

        if (command == kCommandFile) {
            // LOG_DBG, not LOG_INF: a File stream is ~400 of these over
            // ~12s (issue #60) - see QcFileStream.hpp's summary, logged at
            // LOG_INF, for the readable, once-per-stream version of this.
            LOG_DBG("File message: wire=%zu inflated=%zu compressed=%d", wireSize, inflatedSize,
                    compressed);
            ScopedKMutex lock(fileStreamMutex_);
            fileStream_.onMessage(k_uptime_get(), wireSize, inflatedSize, compressed);
        }
    }

    void onInflateFailed(uint16_t command, size_t wireSize, const char* reason) override {
        if (command == kCommandFile) {
            LOG_WRN("File message failed to inflate (wire=%zu bytes): %s", wireSize, reason);
            ScopedKMutex lock(fileStreamMutex_);
            fileStream_.onInflateFailed(k_uptime_get(), wireSize);
        }
    }

    // Called from QcHidBridge::onReportIn() on the `usbh` thread for
    // HidReassembler::Status::Overflow/Malformed (issue #60's
    // hardware-safety follow-up) - atomic increment only, same contract as
    // handleMessage() above: no logging, nothing that can block. These
    // drops happen before a command id is even known, so they can't be
    // attributed to the File stream specifically the way
    // dispatcher_/gzipInflater_'s drops can - see QcFileStream::Summary's
    // comment on reassemblyOverflowCount/reassemblyMalformedCount for why
    // they're still folded into that summary anyway, as a best-effort
    // "this happened somewhere during the stream" figure.
    void recordReassemblyOverflow() { reassemblyOverflowCount_.fetch_add(1, std::memory_order_relaxed); }
    void recordReassemblyMalformed() { reassemblyMalformedCount_.fetch_add(1, std::memory_order_relaxed); }

private:
    // Below the main LED-blink loop's priority; documented since nothing
    // else contends for the CPU yet.
    static constexpr int kThreadPriority = 7;

    // Was 500ms under #7; confirmed on real hardware (issue #8's debugging)
    // that this device's actual Version-reply latency is often several
    // hundred ms, consistently past that window - the handshake's wait
    // logged gotReply=0 while the real reply arrived ~700ms after the
    // send. 1000ms is a margin above the worst case observed, not a value
    // qc-mcp documents (its own per-step collect() windows are shorter,
    // but none of them gates a single request/reply the way this wait
    // does). Confirmed working on hardware at this value: the handshake
    // now mirrors the device's real firmware version instead of falling
    // back.
    static constexpr k_timeout_t kVersionReplyTimeout = K_MSEC(1000);

    // Higher priority than the handshake thread: the handshake runs once
    // and tolerates being delayed, but a late KeepAlive risks the device
    // deciding the session is inactive and stopping its stream (issue #8).
    // Still below the main LED-blink loop's priority.
    static constexpr int kHeartbeatThreadPriority = 5;
    static constexpr k_timeout_t kHeartbeatInterval = K_MSEC(200);

    // Single device, single session at a time - a fixed id is enough (no
    // need for qc-mcp's Python client's random per-run UUID-style value).
    static constexpr std::string_view kSessionId = "qcremote-firmware-0000000000000";

    // qc-mcp mirrors the device's own reported firmware version back in the
    // Version UPDATE (transport.py::_handshake). If the READ reply doesn't
    // carry a usable field, or times out, fall back to a fixed string
    // rather than blocking the handshake forever - mirrors transport.py's
    // f"{LATEST_VERSION}.0" fallback (see issue #5).
    static constexpr std::string_view kFallbackFirmwareVersion = "4.1.0";

    static constexpr uint8_t kReportIdHostToQc = 0x02;

    static void threadEntryTrampoline(void* p1, void*, void*) {
        static_cast<QcSession*>(p1)->run();
    }

    void run() {
        while (true) {
            k_sem_take(&deviceReadySem_, K_FOREVER);
            runHandshake();
        }
    }

    void runHandshake() {
        // Starts the heartbeat first, before any handshake message - see
        // this class's header comment for why.
        startHeartbeat();

        HandshakeMessageBuffer msg;

        encodeResetCommsBuffers(msg, kSessionId);
        if (!sendMessage(msg)) {
            LOG_ERR("Handshake: failed to send ResetCommsBuffers");
            return;
        }

        msg.clear();
        encodeVersionRead(msg);
        pendingVersionReply_.reset();
        waitingForVersionReply_.store(true);
        const bool sent = sendMessage(msg);
        const bool gotReply = sent && k_sem_take(&versionReplySem_, kVersionReplyTimeout) == 0;
        waitingForVersionReply_.store(false);

        if (!sent) {
            LOG_ERR("Handshake: failed to send Version READ");
            return;
        }

        FirmwareVersionString mirroredVersion;
        if (gotReply && pendingVersionReply_ && !pendingVersionReply_->zenosGitHash.empty()) {
            mirroredVersion.assign(pendingVersionReply_->zenosGitHash.data(),
                                   pendingVersionReply_->zenosGitHash.size());
        } else if (gotReply && pendingVersionReply_ && !pendingVersionReply_->appFwVersion.empty()) {
            mirroredVersion.assign(pendingVersionReply_->appFwVersion.data(),
                                   pendingVersionReply_->appFwVersion.size());
        } else {
            mirroredVersion.assign(kFallbackFirmwareVersion.data(), kFallbackFirmwareVersion.size());
            LOG_WRN("No usable Version reply (gotReply=%d), using fallback %s", gotReply,
                    mirroredVersion.c_str());
        }

        msg.clear();
        encodeVersionUpdate(msg, std::string_view(mirroredVersion.data(), mirroredVersion.size()),
                            nextRequestId());
        if (!sendMessage(msg)) {
            LOG_ERR("Handshake: failed to send Version UPDATE");
            return;
        }

        msg.clear();
        encodeConnection(msg, true, nextRequestId());
        if (!sendMessage(msg)) {
            LOG_ERR("Handshake: failed to send Connection UPDATE");
            return;
        }

        LOG_INF("Session handshake complete (firmware version: %s)", mirroredVersion.c_str());

        // File READ (issue #60): asks for the device's whole catalog, which
        // comes back as a stream of File UPDATE messages - see
        // QcFileStream.hpp for how that stream is collected and
        // runHeartbeat() for where it's polled to a finish. Snapshotting
        // the drop/overflow/malformed counters here, to diff against at
        // the stream's finish, scopes those (otherwise lifetime) counts to
        // this stream rather than everything since boot.
        {
            ScopedKMutex lock(fileStreamMutex_);
            fileStream_.start(k_uptime_get());
            dispatcherDroppedAtStart_ = dispatcher_.droppedCount();
            gzipDroppedAtStart_ = gzipInflater_.droppedCount();
            reassemblyOverflowAtStart_ = reassemblyOverflowCount_.load(std::memory_order_relaxed);
            reassemblyMalformedAtStart_ = reassemblyMalformedCount_.load(std::memory_order_relaxed);
        }
        msg.clear();
        encodeFileRead(msg, nextRequestId());
        if (!sendMessage(msg)) {
            LOG_ERR("Handshake: failed to send File READ");
        }
    }

    // Starts (or restarts, on a reconnect) the ~200ms KeepAlive heartbeat
    // (issue #8). Creates the heartbeat thread on first use, same lazy
    // pattern as deviceConnected(); the timer itself only ever signals a
    // semaphore from heartbeatTimerExpiryTrampoline(), never sends directly
    // - k_timer expiry functions run in interrupt-adjacent context, where a
    // blocking SET_REPORT send is forbidden (CODING_STANDARDS.md's ISR
    // safety rules). Called from runHandshake() before any handshake
    // message - see this class's header comment.
    void startHeartbeat() {
        if (!heartbeatThreadStarted_) {
            k_thread_create(&heartbeatThread_, heartbeatStack_, K_KERNEL_STACK_SIZEOF(heartbeatStack_),
                            &heartbeatThreadTrampoline, this, nullptr, nullptr, kHeartbeatThreadPriority,
                            0, K_NO_WAIT);
            k_thread_name_set(&heartbeatThread_, "qc_heartbeat");
            heartbeatThreadStarted_ = true;
            LOG_INF("KeepAlive heartbeat started (~200ms)");
        }
        // K_NO_WAIT for the first expiry: qc-mcp's own heartbeat loop
        // (transport.py::_start_heartbeat) sends immediately on start, then
        // waits the interval - matched here rather than waiting 200ms before
        // the very first KeepAlive.
        k_timer_start(&heartbeatTimer_, K_NO_WAIT, kHeartbeatInterval);
    }

    static void heartbeatTimerExpiryTrampoline(struct k_timer* timer) {
        auto* self = static_cast<QcSession*>(k_timer_user_data_get(timer));
        if (self != nullptr) {
            k_sem_give(&self->heartbeatSem_);
        }
    }

    static void heartbeatThreadTrampoline(void* p1, void*, void*) {
        static_cast<QcSession*>(p1)->runHeartbeat();
    }

    void runHeartbeat() {
        while (true) {
            k_sem_take(&heartbeatSem_, K_FOREVER);
            HandshakeMessageBuffer msg;
            encodeKeepAlive(msg);
            if (!sendMessage(msg)) {
                LOG_WRN("KeepAlive send failed");
            }

            // Piggybacks on the heartbeat's own ~200ms cadence rather than
            // running a separate timer - plenty of resolution against
            // QcFileStream's 1500ms quiet window and 30s overall timeout,
            // and keeps this off the `usbh` thread (see handleMessage()).
            std::optional<QcFileStream::Summary> summary;
            {
                ScopedKMutex lock(fileStreamMutex_);
                summary = fileStream_.checkTimeout(k_uptime_get());
                if (summary) {
                    // dispatcher_.droppedCount()/gzipInflater_.droppedCount() and
                    // the reassembly counters are lifetime totals (see their
                    // own comments) - diff against the snapshot taken when
                    // this stream started so the summary reports only what
                    // happened during it.
                    summary->dispatcherDropped = dispatcher_.droppedCount() - dispatcherDroppedAtStart_;
                    summary->gzipDropped = gzipInflater_.droppedCount() - gzipDroppedAtStart_;
                    summary->reassemblyOverflowCount =
                        reassemblyOverflowCount_.load(std::memory_order_relaxed) - reassemblyOverflowAtStart_;
                    summary->reassemblyMalformedCount =
                        reassemblyMalformedCount_.load(std::memory_order_relaxed) - reassemblyMalformedAtStart_;
                }
            }
            if (summary) {
                LOG_INF("File stream finished (%s): %u messages (%u compressed, %u failed), "
                        "%zu wire bytes, %zu inflated bytes, %d ms",
                        summary->timedOut ? "timeout" : "quiet", summary->messageCount,
                        summary->compressedCount, summary->failedCount, summary->totalWireBytes,
                        summary->totalInflatedBytes, static_cast<int>(summary->elapsedMs));
                // Separate line: sizing (#33) and data-loss figures. A
                // non-zero dropped/overflow/malformed count here is the
                // known gap (see firmware/README.md's issue #60 section) -
                // not necessarily specific to File messages for the
                // reassembly counters (they're counted before a command id
                // is known), but real evidence of loss during this stream's
                // window either way.
                LOG_INF("File stream limits: max wire=%zu max inflated=%zu, dropped "
                        "(dispatch=%u gzip=%u), reassembly (overflow=%u malformed=%u)",
                        summary->maxWireBytes, summary->maxInflatedBytes, summary->dispatcherDropped,
                        summary->gzipDropped, summary->reassemblyOverflowCount,
                        summary->reassemblyMalformedCount);
            }
        }
    }

    // Shared by the handshake thread and the heartbeat thread - sendMutex_
    // keeps their reports from interleaving on the wire (issue #8's "lock
    // around the USB send path" requirement), since a single logical
    // message is often chunked into several reports by HidChunker.
    bool sendMessage(std::span<const uint8_t> message) {
        ScopedKMutex lock(sendMutex_);

        HidChunker chunker(message, kReportIdHostToQc);
        etl::vector<uint8_t, kHidReportSize> report;
        report.resize(kHidReportSize);

        while (chunker.next({report.data(), report.size()})) {
            const int ret = UsbHostClass::sendReport({report.data(), report.size()});
            if (ret != 0 && ret != -EPIPE) {
                LOG_ERR("sendReport failed: %d", ret);
                return false;
            }
            if (ret == -EPIPE) {
                // PROTOCOL.md §1 describes a macOS quirk where
                // IOHIDDeviceSetReport returns a benign-looking error on
                // this device while the data still lands; -EPIPE here is
                // the same thing one layer down. Confirmed functionally on
                // real hardware during issue #8's debugging: every send
                // (handshake and heartbeat alike) returns -EPIPE, yet
                // repeated Version READs reliably got answered for
                // several minutes with the heartbeat running - the data
                // demonstrably lands despite the reported error. The exact
                // USB-level mechanism (status-stage-only stall vs. the
                // controller's own reporting quirk) is still not
                // independently verified against the driver, but the
                // functional outcome is.
                LOG_DBG("sendReport: -EPIPE (documented benign quirk, PROTOCOL.md §1) - "
                        "continuing");
            }
        }
        return true;
    }

    uint64_t nextRequestId() { return ++requestIdCounter_; }

    K_KERNEL_STACK_MEMBER(stack_, 2048);
    struct k_thread thread_ {};
    bool threadStarted_ = false;

    K_KERNEL_STACK_MEMBER(heartbeatStack_, 2048);
    struct k_thread heartbeatThread_ {};
    bool heartbeatThreadStarted_ = false;
    struct k_timer heartbeatTimer_ {};
    struct k_sem heartbeatSem_ {};

    struct k_mutex sendMutex_ {};

    struct k_sem deviceReadySem_ {};
    struct k_sem versionReplySem_ {};
    std::atomic<bool> waitingForVersionReply_{false};
    std::optional<VersionReply> pendingVersionReply_;
    uint64_t requestIdCounter_ = 0;

    QcDispatcher dispatcher_;
    QcGzipInflater gzipInflater_;

    // Written from QcHidBridge::onReportIn() on the `usbh` thread (atomic
    // increment only - see recordReassemblyOverflow()/recordReassemblyMalformed()
    // above), read from runHeartbeat() when assembling a File stream
    // summary. Lifetime counts - runHandshake() snapshots them into
    // *AtStart_ below to scope a summary to one stream.
    std::atomic<uint32_t> reassemblyOverflowCount_{0};
    std::atomic<uint32_t> reassemblyMalformedCount_{0};

    // Guards fileStream_ and the four *AtStart_ snapshots below: all are
    // written from runHandshake() at stream start, read/diffed from
    // runHeartbeat() at stream finish, and fileStream_ is also written
    // from onMessage()/onInflateFailed() (the dispatcher_/gzipInflater_
    // worker threads) - never from `usbh`.
    struct k_mutex fileStreamMutex_ {};
    QcFileStream fileStream_;
    uint32_t dispatcherDroppedAtStart_ = 0;
    uint32_t gzipDroppedAtStart_ = 0;
    uint32_t reassemblyOverflowAtStart_ = 0;
    uint32_t reassemblyMalformedAtStart_ = 0;
};

}  // namespace qcbridge
