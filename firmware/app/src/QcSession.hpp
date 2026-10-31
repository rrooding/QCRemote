#pragma once

#include <atomic>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <etl/vector.h>

#include "HidChunker.hpp"
#include "HidReassembler.hpp"
#include "QcHandshakeMessages.hpp"
#include "QcMessage.hpp"
#include "ScopedKMutex.hpp"
#include "UsbHostClass.hpp"

namespace qcbridge {

// Runs the connect-time handshake (issue #7: ResetCommsBuffers, Version,
// Connection - see PROTOCOL.md section 3, steps 1-3) once a device is
// claimed.
//
// This needs its own thread. qc_usbh_send_report() is now a *blocking*
// control transfer (up to Zephyr's 5s setup-request timeout) that only
// completes once the USB host stack's own thread finishes processing it -
// and that's the same thread that calls onProbe()/onReportIn(). Sending
// from either of those callbacks directly would deadlock the host stack
// against itself. So QcHidBridge only ever *signals* this class (via
// semaphores, never blocking); the actual sends happen on a dedicated
// thread instead.
//
// Also runs the ~200ms KeepAlive heartbeat (issue #8) once the handshake
// completes, on its own dedicated thread/timer so a stalled or slow
// handshake/heartbeat send never blocks the other. sendMessage() is shared
// by both and guarded by sendMutex_ so their HID reports can never
// interleave on the wire.
//
// General message dispatch beyond recognizing a Version reply (issue #5
// Tier 2) is still out of scope here, see issue #7's body.
class QcSession {
public:
    QcSession() {
        k_sem_init(&deviceReadySem_, 0, 1);
        k_sem_init(&versionReplySem_, 0, 1);
        k_sem_init(&heartbeatSem_, 0, 1);
        k_mutex_init(&sendMutex_);
        k_timer_init(&heartbeatTimer_, &heartbeatTimerExpiryTrampoline, nullptr);
        k_timer_user_data_set(&heartbeatTimer_, this);
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
        k_sem_give(&deviceReadySem_);
    }

    // Called from onReportIn() (host stack thread) once HidReassembler has
    // completed a message. Only a Version reply is meaningful to the
    // handshake right now; everything else is dropped - general dispatch
    // is issue #5 Tier 2 work, not this issue's.
    //
    // Deliberately does no logging of its own (a prior build's diagnostic
    // LOG_INF here crashed real hardware with a stack overflow - see
    // runHandshake()'s comment on lastUnexpected*_ below for the full
    // story). This runs on Zephyr's own internal `usbh` thread
    // (CONFIG_USBH_STACK_SIZE, 1024 bytes by default), not one of ours -
    // any logging call's cbprintf formatting depth (CONFIG_LOG_MODE_IMMEDIATE
    // runs it synchronously on the calling thread) is unsafe to add here
    // without separately confirmed headroom.
    void handleMessage(std::span<const uint8_t> message) {
        const auto decoded = decodeMessage(message);
        if (!decoded) {
            return;
        }

        // Unconditional, plain-array stash (no logging on this thread - see
        // this function's header comment) of every decoded message's
        // command/size, regardless of which branch below handles it. Issue
        // #8's hardware report found a recurring 331/10(/14-once) byte
        // pattern after every probe read and couldn't tell what those
        // messages actually are beyond size; this gives the next report
        // real command ids to check that against, and whether e.g. two
        // separate command-10 messages genuinely arrive per cycle or
        // something is double-counting.
        {
            const uint32_t idx = recentMessageCount_.fetch_add(1) % kRecentMessageLogSize;
            recentMessages_[idx] = {decoded->command, static_cast<uint16_t>(message.size())};
        }

        if (decoded->command == kCommandVersion && waitingForVersionReply_.load()) {
            pendingVersionReply_ = decodeVersionReply(decoded->protoBytes);
            k_sem_give(&versionReplySem_);
        } else if (waitingForVersionReply_.load()) {
            // Stash only - see runHandshake() for why this is reported from
            // the handshake thread instead of logged right here. Snapshot
            // a few proto bytes too (plain memcpy, no formatting) so the
            // next hardware report can show whether this is a real
            // ResetCommsBuffers ack (e.g. an echoed session_id) or an
            // unrelated/unsolicited message that merely happens to share
            // its command id - undecidable from command id and length
            // alone (see firmware/README.md's KeepAlive section).
            const size_t snapshotLen =
                decoded->protoBytes.size() < kUnexpectedSnapshotSize ? decoded->protoBytes.size()
                                                                     : kUnexpectedSnapshotSize;
            std::memcpy(lastUnexpectedSnapshot_, decoded->protoBytes.data(), snapshotLen);
            lastUnexpectedSnapshotLen_ = snapshotLen;
            lastUnexpectedCommand_.store(decoded->command);
            lastUnexpectedSize_.store(message.size());
            hasUnexpectedMessage_.store(true);
        } else if (decoded->command == kCommandVersion) {
            // A Version reply arriving after the wait above already closed
            // (kVersionReplyTimeout) - issue #8's hardware report always
            // saw gotReply=0, so this tells the next report whether our
            // Version READ is ever actually answered at all, just late, or
            // genuinely never. Counted (not stashed-and-logged-once) since
            // this can fire repeatedly; reported periodically from
            // runHeartbeat(), same reasoning as every other counter there.
            lateVersionReplyCount_.fetch_add(1);
        }
    }

private:
    // Below the main LED-blink loop's priority; documented since nothing
    // else contends for the CPU yet.
    static constexpr int kThreadPriority = 7;

    // Was 500ms; bumped after issue #8's hardware report showed the actual
    // device latency for a Version reply is often close to or past that -
    // the handshake's own wait and several periodic probe reads all came
    // back gotReply=0, yet a response consistently showed up on the wire
    // ~200-700ms later (recurring 331/10-byte messages right after each
    // Version READ - see firmware/README.md's KeepAlive section). This is
    // a real latency margin problem, not confirmed to be anything else;
    // 1000ms gives headroom above the worst observed case rather than
    // being derived from a spec value qc-mcp doesn't document either
    // (transport.py's own per-step collect() windows are much shorter,
    // but it isn't gating a single request/reply the way this wait is).
    static constexpr k_timeout_t kVersionReplyTimeout = K_MSEC(1000);

    // Higher priority than the handshake thread: the handshake runs once
    // and tolerates being delayed, but a late KeepAlive risks the device
    // deciding the session is inactive and stopping its stream (issue #8).
    // Still below the main LED-blink loop's priority.
    static constexpr int kHeartbeatThreadPriority = 5;
    static constexpr k_timeout_t kHeartbeatInterval = K_MSEC(200);

    // Every 25 sends (~5s at 200ms) rather than every send - issue #8's
    // hardware report found a plain "it sent, nothing logged" heartbeat
    // indistinguishable on the console from a hung one, since a successful
    // or -EPIPE'd send (the documented quirk, see sendMessage() below)
    // logs nothing by design. This is diagnostic visibility, not meant to
    // be log-noisy at the full 5/sec cadence.
    static constexpr uint32_t kHeartbeatLogInterval = 25;

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
        // Starts the heartbeat FIRST, before any handshake message - matching
        // qc-mcp's own transport.py::open() exactly (`_start_heartbeat()`
        // then `_handshake()`, with the comment "keep session online
        // (required for reads)" on the heartbeat call). Our original
        // ordering did this backwards (heartbeat only after the full
        // handshake completed), discovered while investigating issue #8's
        // hardware report that Version READ never got a reply even though
        // PROTOCOL.md says Version is answered "from any session" - that
        // claim may only hold once something is already keeping the
        // session alive, which nothing was yet under the old ordering.
        startHeartbeat();

        HandshakeMessageBuffer msg;

        encodeResetCommsBuffers(msg, kSessionId);
        const bool sentReset = sendMessage(msg);
        LOG_INF("Handshake: ResetCommsBuffers send ok=%d last ret=%d", sentReset,
                lastSendReportRet_);
        if (!sentReset) {
            LOG_ERR("Handshake: failed to send ResetCommsBuffers");
            return;
        }

        msg.clear();
        encodeVersionRead(msg);
        pendingVersionReply_.reset();
        hasUnexpectedMessage_.store(false);
        waitingForVersionReply_.store(true);
        const bool sent = sendMessage(msg);
        LOG_INF("Handshake: Version READ send ok=%d last ret=%d", sent, lastSendReportRet_);
        const bool gotReply = sent && k_sem_take(&versionReplySem_, kVersionReplyTimeout) == 0;
        waitingForVersionReply_.store(false);

        if (!sent) {
            LOG_ERR("Handshake: failed to send Version READ");
            return;
        }

        // Diagnostic for issue #8's hardware report: a hardware trace
        // showed a message arriving during this exact wait yet still
        // logged "No usable Version reply (gotReply=0)" below - this
        // settles whether that message actually was the Version reply
        // (command == kCommandVersion, just too late for
        // kVersionReplyTimeout - a real #7 latency bug) or something else
        // entirely. Logged here (handshake thread, 2048-byte stack, already
        // proven safe for LOG_* calls on real hardware - see #7's verified
        // trace) rather than from handleMessage() itself - see that
        // function's comment for why logging there crashed a previous
        // diagnostic build.
        if (hasUnexpectedMessage_.load()) {
            LOG_INF("Message while awaiting Version reply: command=%u, %zu bytes",
                    lastUnexpectedCommand_.load(), lastUnexpectedSize_.load());
            LOG_HEXDUMP_INF(lastUnexpectedSnapshot_, lastUnexpectedSnapshotLen_,
                             "unexpected message proto bytes (truncated)");
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
        const bool sentVersionUpdate = sendMessage(msg);
        LOG_INF("Handshake: Version UPDATE send ok=%d last ret=%d", sentVersionUpdate,
                lastSendReportRet_);
        if (!sentVersionUpdate) {
            LOG_ERR("Handshake: failed to send Version UPDATE");
            return;
        }

        msg.clear();
        encodeConnection(msg, true, nextRequestId());
        const bool sentConnection = sendMessage(msg);
        LOG_INF("Handshake: Connection UPDATE send ok=%d last ret=%d", sentConnection,
                lastSendReportRet_);
        if (!sentConnection) {
            LOG_ERR("Handshake: failed to send Connection UPDATE");
            return;
        }

        LOG_INF("Session handshake complete (firmware version: %s)", mirroredVersion.c_str());
    }

    // Starts (or restarts, on a reconnect) the ~200ms KeepAlive heartbeat
    // (issue #8). Creates the heartbeat thread on first use, same lazy
    // pattern as deviceConnected(); the timer itself only ever signals a
    // semaphore from heartbeatTimerExpiryTrampoline(), never sends directly
    // - k_timer expiry functions run in interrupt-adjacent context, where a
    // blocking SET_REPORT send is forbidden (CODING_STANDARDS.md's ISR
    // safety rules). Called from runHandshake() before any handshake
    // message now, not after - see that function's comment.
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

            // A successful or -EPIPE'd send (the documented quirk) logs
            // nothing on its own, so a hung/stalled heartbeat would be
            // silent too - this periodic line is the only console evidence
            // that the heartbeat thread is actually still ticking (issue
            // #8's hardware report had no way to tell). inTransferCount()
            // alongside it shows whether the receive loop is still being
            // driven at all, independent of whether any message has been
            // reassembled - sendReport() and on_report_in() share the USB
            // host stack's one processing thread (usbh_shim.h), so a send
            // every ~200ms contending with it is exactly the kind of thing
            // this is meant to catch.
            if (++heartbeatSendCount_ % kHeartbeatLogInterval == 0) {
                const bool probeReplied = probeVersionRead();
                LOG_INF("KeepAlive heartbeat alive: %u sent, last sendReport ret=%d, "
                        "%u IN transfers, %u late Version replies, probe read replied=%d",
                        heartbeatSendCount_, lastSendReportRet_, UsbHostClass::inTransferCount(),
                        lateVersionReplyCount_.load(), probeReplied);
                // Ring buffer slots in fixed index order, not strictly
                // chronological (the next write wraps to slot 0 regardless
                // of how many total messages have landed) - good enough to
                // see which command ids/sizes are actually recurring.
                LOG_INF("Recent messages (command:size) slots 0-3: "
                        "%u:%u %u:%u %u:%u %u:%u",
                        recentMessages_[0].command, recentMessages_[0].size,
                        recentMessages_[1].command, recentMessages_[1].size,
                        recentMessages_[2].command, recentMessages_[2].size,
                        recentMessages_[3].command, recentMessages_[3].size);
            }
        }
    }

    // Sends a Version READ and waits for a reply, same as the handshake's
    // own Version READ step - a direct test of "does a read get answered
    // while the heartbeat is running," the exact question issue #8's
    // hardware investigation needs settled (qc-mcp's own control check
    // confirmed reads work under *its* heartbeat; this checks ours the
    // same way). Reuses the handshake's wait/reply plumbing - safe because
    // this only ever runs after the handshake thread's own one-time use of
    // it has already finished (startHeartbeat() happens before the
    // handshake's sends now, but the heartbeat thread doesn't reach its
    // first kHeartbeatLogInterval tick - several seconds in - until long
    // after runHandshake() has returned from its own wait).
    bool probeVersionRead() {
        HandshakeMessageBuffer msg;
        encodeVersionRead(msg);
        pendingVersionReply_.reset();
        waitingForVersionReply_.store(true);
        const bool sent = sendMessage(msg);
        const bool gotReply = sent && k_sem_take(&versionReplySem_, kVersionReplyTimeout) == 0;
        waitingForVersionReply_.store(false);
        return gotReply;
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
            lastSendReportRet_ = ret;
            if (ret != 0 && ret != -EPIPE) {
                LOG_ERR("sendReport failed: %d", ret);
                return false;
            }
            if (ret == -EPIPE) {
                // PROTOCOL.md §1 describes a macOS quirk where
                // IOHIDDeviceSetReport returns a benign-looking error on
                // this device while the data still lands. #7 assumed
                // -EPIPE here was the same thing one layer down, reasoning
                // from indirect evidence (the handshake's post-connect
                // burst happening afterward). Issue #8's hardware testing
                // put real doubt on that: every KeepAlive send also returns
                // -EPIPE (850+ in a row, one real run), yet the device
                // shows zero effect of any kind - no new IN transfers, no
                // reply to a repeated preset-change test. qc-mcp's own
                // transport.py has a telling comment on this exact
                // ambiguity: a write error that looks benign on an
                // exclusive handle is "exactly what a write that never
                // reaches the device returns" on a shared one - the same
                // error code, two different real meanings, context-
                // dependent. Zephyr's NXP EHCI driver (uhc_mcux_ehci.c)
                // doesn't help settle it either: -EPIPE is a single
                // pass/fail result for the *whole* control transfer, and
                // transferSofar is only tracked for IN-direction transfers
                // (this is OUT), so there's no byte count to check even in
                // principle. Treating -EPIPE as non-fatal (continuing
                // rather than erroring out) is still the pragmatic choice -
                // see firmware/README.md's KeepAlive section for the
                // current state of this investigation and why a stricter
                // treatment hasn't replaced it (not enough to confirm
                // either reading yet).
                LOG_DBG("sendReport: -EPIPE (benign per #7, now unconfirmed - see "
                        "firmware/README.md) - continuing");
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
    uint32_t heartbeatSendCount_ = 0;
    int lastSendReportRet_ = 0;

    struct k_mutex sendMutex_ {};

    struct k_sem deviceReadySem_ {};
    struct k_sem versionReplySem_ {};
    std::atomic<bool> waitingForVersionReply_{false};
    std::optional<VersionReply> pendingVersionReply_;
    uint64_t requestIdCounter_ = 0;

    // Written from handleMessage() (the usbh thread), read/cleared from
    // runHandshake() (the handshake thread) - see both for why this is a
    // plain stash rather than a log call made where it's detected.
    std::atomic<bool> hasUnexpectedMessage_{false};
    std::atomic<uint16_t> lastUnexpectedCommand_{0};
    std::atomic<size_t> lastUnexpectedSize_{0};
    // Plain (non-atomic) snapshot bytes - safe because hasUnexpectedMessage_
    // is written last on the producer side and checked first on the
    // consumer side, so its seq_cst store/load pair acts as the handoff.
    static constexpr size_t kUnexpectedSnapshotSize = 16;
    uint8_t lastUnexpectedSnapshot_[kUnexpectedSnapshotSize] = {};
    size_t lastUnexpectedSnapshotLen_ = 0;

    // Written from handleMessage() (usbh thread), read from runHeartbeat()
    // (heartbeat thread) - see handleMessage() for what this counts.
    std::atomic<uint32_t> lateVersionReplyCount_{0};

    // Written from handleMessage() (usbh thread), read from runHeartbeat()
    // (heartbeat thread) - plain array, not atomic per-element (same
    // accepted-race reasoning as lastUnexpectedSnapshot_ above; this is
    // diagnostic-only, a torn read just shows a stale/mixed slot once).
    struct RecentMessage {
        uint16_t command = 0;
        uint16_t size = 0;
    };
    static constexpr size_t kRecentMessageLogSize = 4;
    RecentMessage recentMessages_[kRecentMessageLogSize] = {};
    std::atomic<uint32_t> recentMessageCount_{0};
};

}  // namespace qcbridge
