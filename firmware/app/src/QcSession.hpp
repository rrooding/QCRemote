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
#include "QcFileMessage.hpp"
#include "QcFileRequest.hpp"
#include "QcFileStream.hpp"
#include "QcGzip.hpp"
#include "QcGzipInflater.hpp"
#include "QcHandshakeMessages.hpp"
#include "QcMessage.hpp"
#include "QcMessageSink.hpp"
#include "QcMessageTally.hpp"
#include "QcPresetCatalog.hpp"
#include "QcPresetPrinter.hpp"
#include "QcStateReads.hpp"
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
        // issue #10 Copilot review: presetPrinter_ needs the same mutex
        // every write to presetCatalog_ already goes through, to copy a
        // folder's data out before printing it - see
        // QcPresetPrinter::printCatalog()'s own comment.
        presetPrinter_.setCatalogMutex(fileStreamMutex_);
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
        presetPrinter_.start();
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

        // issue #33 (ADR 0007): ModelRepo/ModuleStats replies can be tens
        // of KB (a real hardware run saw ModelRepo at 59219 bytes,
        // ModuleStats at 28354) and the preset listing doesn't need them -
        // issue #61's post-connect state READs (see runHandshake()) just
        // need to go out, not be buffered or dispatched here. Counted and
        // dropped before the gzip-magic check or either worker's mailbox,
        // so a reply to either can never consume the dispatcher/gzip-
        // inflater's single in-flight slot - see checkTrafficHealth()'s
        // comment on the gzip-mailbox drops a full (pre-omit_factory_content)
        // catalog listing used to show. Atomic increment only - usbh-safe,
        // same contract as the rest of this function.
        if (decoded->command == kCommandModelRepo || decoded->command == kCommandModuleStats) {
            ignoredLargeCommandCount_.fetch_add(1, std::memory_order_relaxed);
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
            // LOG_DBG, not LOG_INF: a File stream is ~20 of these over a
            // few seconds (issue #33: omit_factory_content=true shrank it
            // from issue #60's ~550) - see QcFileStream.hpp's summary,
            // logged at LOG_INF, for the readable, once-per-stream version
            // of this.
            LOG_DBG("File message: wire=%zu inflated=%zu compressed=%d", wireSize, inflatedSize,
                    compressed);
        }

        ScopedKMutex lock(fileStreamMutex_);
        // postStreamTally_ records every command, not just File (issue
        // #61) - see its own start() call site for why.
        postStreamTally_.record(command);
        if (command == kCommandFile && fileStream_.isActive()) {
            fileStream_.onMessage(k_uptime_get(), wireSize, inflatedSize, compressed);
            // issue #10/#9: decodeFile()'s FilePreset fields are views into
            // `protoBytes`, which belongs to the dispatcher/gzip-inflater
            // mailbox this callback was invoked from and gets overwritten by
            // the next message - presetCatalog_.addPreset() copies what it
            // keeps before this call returns (QcPresetCatalog.hpp). Gating on
            // fileStream_.isActive() under this same lock is also what keeps
            // the catalog quiescent the moment the stream finishes, which is
            // what lets QcPresetPrinter.hpp read it without a lock of its own.
            decodeFile(protoBytes, [this](const FilePreset& preset) { presetCatalog_.addPreset(preset); });
        }
    }

    void onInflateFailed(uint16_t command, size_t wireSize, const char* reason,
                          std::optional<size_t> advertisedInflatedSize) override {
        if (command == kCommandFile) {
            LOG_WRN("File message failed to inflate (wire=%zu bytes): %s", wireSize, reason);
        }

        ScopedKMutex lock(fileStreamMutex_);
        postStreamTally_.record(command);
        if (command == kCommandFile && fileStream_.isActive()) {
            fileStream_.onInflateFailed(k_uptime_get(), wireSize, advertisedInflatedSize);
        }
    }

    // Called from QcHidBridge::onReportIn() on the `usbh` thread for
    // HidReassembler::Status::Overflow - atomic increment only, same
    // contract as handleMessage() above: no logging, nothing that can
    // block. Folded into the File stream's summary (QcFileStream::Summary's
    // reassemblyOverflowCount) at stream finish, and watched continuously by
    // checkTrafficHealth() (issue #33 / ADR 0007) so an overflow after the
    // stream is done - e.g. a preset push too big for kReassemblyCapacity -
    // is still visible, not just the known gap during the listing itself.
    void recordReassemblyOverflow(size_t trueSize) {
        reassemblyOverflowCount_.fetch_add(1, std::memory_order_relaxed);
        // Single writer (usbh) - no race to arbitrate, same reasoning as
        // reassemblyOverflowCount_'s own fetch_add. A plain load-then-store
        // instead of compare_exchange is safe because only this thread ever
        // writes this value.
        if (trueSize > reassemblyOverflowMaxSize_.load(std::memory_order_relaxed)) {
            reassemblyOverflowMaxSize_.store(static_cast<uint32_t>(trueSize), std::memory_order_relaxed);
        }
    }
    void recordReassemblyMalformed() { reassemblyMalformedCount_.fetch_add(1, std::memory_order_relaxed); }

    // Called from QcHidBridge::onReportIn() on the `usbh` thread for every
    // successful IN transfer, regardless of what the reassembler does with
    // it (issue #60's hardware debugging: a run that shows 0 File messages
    // needs this to tell "the device never sent a single report" apart from
    // "it sent reports that just never turned into a counted message").
    // Same atomic-increment-only contract as the two above.
    void recordInTransfer() { totalInTransfers_.fetch_add(1, std::memory_order_relaxed); }

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

        // Post-connect state READs (issue #61): qc-mcp's own
        // transport.py::_handshake() sends these twelve READs right here -
        // after its Connection UPDATE, before any listing - with a comment
        // that this is what "makes the QC start streaming and marks the
        // session active." Sent back-to-back with no wait for a reply in
        // between (and no wait for any of their replies before moving on to
        // the File READ below either): that's what qc-mcp itself does - its
        // loop fires all twelve, then only pauses briefly before discarding
        // whatever already came back, never gating on a specific reply.
        // This firmware mirrors that fire-and-forget shape rather than
        // inventing a synchronous wait qc-mcp doesn't have. A send failure
        // here doesn't abort the sequence - matching qc-mcp's own per-READ
        // try/except - since one state READ failing to go out shouldn't
        // block the rest or the File READ that follows; the replies (if
        // any) are handled by the same generic dispatch path as every other
        // message (QcDispatcher.hpp/QcGzipInflater.hpp), except ModelRepo
        // and ModuleStats, which handleMessage() now ignores outright
        // (issue #33 / ADR 0007) - the preset listing below doesn't need
        // either, and both are large enough that buffering them would cost
        // real RAM for no benefit.
        for (const uint16_t command : kPostConnectStateReadCommands) {
            msg.clear();
            encodeStateRead(msg, command, nextRequestId());
            if (!sendMessage(msg)) {
                LOG_WRN("Handshake: failed to send state READ (command=%u)", command);
            }
        }

        // File READ (issue #60): asks for the device's whole catalog, which
        // comes back as a stream of File UPDATE messages - see
        // QcFileStream.hpp for how that stream is collected and
        // runHeartbeat() for where it's polled to a finish. Snapshotting
        // the drop/overflow/malformed counters here, to diff against at
        // the stream's finish, scopes those (otherwise lifetime) counts to
        // this stream rather than everything since boot.
        //
        // issue #33 (ADR 0007): always sent with omit_factory_content=true
        // (QcFileRequest.hpp) - a hardware measurement found this is the
        // option that actually shrinks the listing (568 messages/~716KB
        // down to 20/~30KB on the owner's unit) while still naming every
        // user/cloud preset; a sibling option, short_listing, measured no
        // effect at all and is no longer sent.
        {
            ScopedKMutex lock(fileStreamMutex_);
            fileStream_.start(k_uptime_get());
            // issue #10: a new listing (first connect or a reconnect, both
            // reach here via runHandshake()) must not carry over a previous
            // one's presets.
            presetCatalog_.reset();
            dispatcherDroppedAtStart_ = dispatcher_.droppedCount();
            gzipDroppedAtStart_ = gzipInflater_.droppedCount();
            reassemblyOverflowAtStart_ = reassemblyOverflowCount_.load(std::memory_order_relaxed);
            reassemblyMalformedAtStart_ = reassemblyMalformedCount_.load(std::memory_order_relaxed);
            totalInTransfersAtStart_ = totalInTransfers_.load(std::memory_order_relaxed);
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
            // QcFileStream's 5000ms quiet window and 30s overall timeout,
            // and keeps this off the `usbh` thread (see handleMessage()).
            std::optional<QcFileStream::Summary> summary;
            uint32_t totalInTransfersThisStream = 0;
            bool postStreamChanged = false;
            std::array<QcMessageTally::Entry, QcMessageTally::kMaxDistinctCommands> postStreamEntries{};
            size_t postStreamEntryCount = 0;
            uint32_t postStreamOtherCount = 0;
            {
                ScopedKMutex lock(fileStreamMutex_);
                summary = fileStream_.checkTimeout(k_uptime_get());
                if (summary) {
                    totalInTransfersThisStream =
                        totalInTransfers_.load(std::memory_order_relaxed) - totalInTransfersAtStart_;
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

                if (postStreamTally_.isActive()) {
                    const auto entries = postStreamTally_.entries();
                    const auto otherCount = postStreamTally_.otherCount();
                    const int64_t nowMs = k_uptime_get();
                    const bool intervalDue =
                        (nowMs - postStreamLastLoggedAtMs_) >= kPostStreamLogIntervalMs;

                    // Only ever print a command that's brand new (the actual
                    // preset-change signal - reported immediately) or whose
                    // count moved since it was last printed (reported at
                    // most once per kPostStreamLogIntervalMs) - never the
                    // whole tracked set on every tick. Without the per-entry
                    // filter, GlobalTempo's own ~4/s ticking (qc-mcp's
                    // documented streaming-under-heartbeat behavior) bumps
                    // *a* count on practically every heartbeat, which used
                    // to reprint every other already-stable entry
                    // (RecallPreset, Scene, Grid, ...) right along with it.
                    for (size_t i = 0; i < entries.size(); ++i) {
                        const bool isNew = i >= postStreamLoggedEntryCount_;
                        const bool bumped = !isNew && entries[i].count != postStreamLoggedEntries_[i].count;
                        if (isNew || (bumped && intervalDue)) {
                            postStreamEntries[postStreamEntryCount] = entries[i];
                            ++postStreamEntryCount;
                            postStreamChanged = true;
                        }
                    }
                    if (otherCount != postStreamLoggedOtherCount_ && intervalDue) {
                        postStreamOtherCount = otherCount;
                        postStreamChanged = true;
                    }

                    if (postStreamChanged) {
                        for (size_t i = 0; i < entries.size(); ++i) {
                            postStreamLoggedEntries_[i] = entries[i];
                        }
                        postStreamLoggedEntryCount_ = entries.size();
                        postStreamLoggedOtherCount_ = otherCount;
                        postStreamLastLoggedAtMs_ = nowMs;
                    }
                }
            }
            if (summary) {
                logFileStreamSummary(*summary, totalInTransfersThisStream);
            }
            // issue #33 / ADR 0007 (Copilot review): checked every tick,
            // not just when the File stream finishes - see this function's
            // own comment.
            checkTrafficHealth();
            if (postStreamChanged) {
                for (size_t i = 0; i < postStreamEntryCount; ++i) {
                    LOG_INF("Post-connect tally: command=%u count=%u", postStreamEntries[i].command,
                            postStreamEntries[i].count);
                }
                if (postStreamOtherCount > 0) {
                    LOG_INF("Post-connect tally: %u messages from other distinct commands "
                            "(> %zu tracked)",
                            postStreamOtherCount, QcMessageTally::kMaxDistinctCommands);
                }
            }
        }
    }

    // Logs the File stream's one-line-pair finish summary (issue #33's
    // final, concise shape - the per-command/per-folder measurement
    // breakdowns this issue used to pick real sizes are gone now that
    // they're picked; see ADR 0007) and starts issue #61's post-connect
    // watch. Called from runHeartbeat() only when a stream just finished.
    void logFileStreamSummary(const QcFileStream::Summary& summary, uint32_t totalInTransfersThisStream) {
        LOG_INF("File stream finished (%s): %u messages (%u compressed, %u failed), %zu wire "
                "bytes, %zu inflated bytes, %d ms",
                summary.timedOut ? "timeout" : "quiet", summary.messageCount, summary.compressedCount,
                summary.failedCount, summary.totalWireBytes, summary.totalInflatedBytes,
                static_cast<int>(summary.elapsedMs));
        // Dropped/overflow/malformed counts above zero here mean the
        // listing is incomplete (issue #33's safety requirement) - not
        // necessarily specific to File messages for the reassembly counters
        // (they're counted before a command id is known), but real evidence
        // of loss during this stream's window either way.
        LOG_INF("File stream limits: max wire=%zu max inflated=%zu, dropped (dispatch=%u gzip=%u), "
                "reassembly (overflow=%u malformed=%u), transfers=%u",
                summary.maxWireBytes, summary.maxInflatedBytes, summary.dispatcherDropped,
                summary.gzipDropped, summary.reassemblyOverflowCount, summary.reassemblyMalformedCount,
                totalInTransfersThisStream);

        // issue #10 Copilot review: see QcFileStream::isListingIncomplete()
        // for the full reasoning, in particular why summary.timedOut is
        // included - a 30s overall timeout can cut the stream off while
        // the device is still sending, and nothing else here counts what's
        // lost that way.
        const bool incomplete = QcFileStream::isListingIncomplete(summary, presetCatalog_.overflowed());
        presetPrinter_.requestPrint(presetCatalog_, incomplete);

        LOG_INF("Post-connect tally started: watching for preset-change pushes "
                "(RecallPreset/Scene/SetlistPosition/...) - issue #61");
        // issue #61's preset-change check needs visibility into whatever
        // arrives *after* the File stream, too (a preset recall on the unit
        // pushing RecallPreset/Scene/SetlistPosition or similar) -
        // postStreamTally_ picks up right here and keeps running for the
        // rest of the session; see the "changed since last logged" check in
        // runHeartbeat() for how it's reported without flooding the log if
        // the device turns out to stream something continuously (qc-mcp's
        // own comment: GlobalTempo/CPULoad stream ~3/s under an active
        // heartbeat, with or without a preset change).
        ScopedKMutex lock(fileStreamMutex_);
        postStreamTally_.start();
        postStreamLoggedEntries_ = {};
        postStreamLoggedEntryCount_ = 0;
        postStreamLoggedOtherCount_ = 0;
        postStreamLastLoggedAtMs_ = k_uptime_get();
    }

    // issue #33 / ADR 0007 (Copilot PR review): dispatcherDropped_/
    // gzipDropped_/reassemblyOverflowCount_/reassemblyMalformedCount_ are
    // lifetime atomics, written from usbh and the dispatcher/gzip-inflate
    // worker threads - logFileStreamSummary() above only reports a snapshot
    // diffed to the File stream's own window, so an oversized message
    // arriving *after* that stream finishes (a big preset push, say) would
    // otherwise never be reported. Called every heartbeat tick and only
    // logs when something has changed since the last time this ran, so a
    // steady state doesn't flood the log. Each counter is read
    // independently (no multi-field struct to catch mid-update), so
    // there's nothing here for a reader to observe half-written - the
    // values are plain, single-field lifetime counts throughout.
    void checkTrafficHealth() {
        const uint32_t dispatcherDropped = dispatcher_.droppedCount();
        const uint32_t gzipDropped = gzipInflater_.droppedCount();
        const uint32_t reassemblyOverflow = reassemblyOverflowCount_.load(std::memory_order_relaxed);
        const uint32_t reassemblyOverflowMax = reassemblyOverflowMaxSize_.load(std::memory_order_relaxed);
        const uint32_t reassemblyMalformed = reassemblyMalformedCount_.load(std::memory_order_relaxed);

        if (dispatcherDropped == lastReportedDispatcherDropped_ &&
            gzipDropped == lastReportedGzipDropped_ &&
            reassemblyOverflow == lastReportedReassemblyOverflow_ &&
            reassemblyMalformed == lastReportedReassemblyMalformed_) {
            return;
        }

        LOG_WRN("Traffic drop/overflow: dropped (dispatch=%u gzip=%u), reassembly (overflow=%u "
                "largest=%u malformed=%u) - a listing or push in progress may be incomplete",
                dispatcherDropped, gzipDropped, reassemblyOverflow, reassemblyOverflowMax,
                reassemblyMalformed);

        lastReportedDispatcherDropped_ = dispatcherDropped;
        lastReportedGzipDropped_ = gzipDropped;
        lastReportedReassemblyOverflow_ = reassemblyOverflow;
        lastReportedReassemblyMalformed_ = reassemblyMalformed;
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

    // issue #10/#9: populated from onMessage() (dispatcher thread) while
    // fileStream_ is active, reset at the same point fileStream_.start() is
    // called (runHandshake()), and printed by presetPrinter_ once the
    // stream finishes - see QcPresetCatalog.hpp/QcPresetPrinter.hpp for the
    // full thread-safety reasoning.
    QcPresetCatalog presetCatalog_;
    QcPresetPrinter presetPrinter_;

    // Written from QcHidBridge::onReportIn() on the `usbh` thread (atomic
    // increment only - see recordReassemblyOverflow()/recordReassemblyMalformed()
    // above), read from runHeartbeat()/checkTrafficHealth(). Lifetime counts
    // - runHandshake() snapshots them into *AtStart_ below to scope a
    // summary to one stream; checkTrafficHealth() watches the lifetime
    // values directly so a later overflow/malformed report is never missed.
    std::atomic<uint32_t> reassemblyOverflowCount_{0};
    std::atomic<uint32_t> reassemblyOverflowMaxSize_{0};
    std::atomic<uint32_t> reassemblyMalformedCount_{0};

    // Written from QcHidBridge::onReportIn() on the `usbh` thread (atomic
    // increment only, same contract as the two above) - one per successful
    // IN transfer, independent of the reassembler's verdict on it.
    std::atomic<uint32_t> totalInTransfers_{0};

    // issue #33 / ADR 0007: ModelRepo/ModuleStats replies ignored outright
    // in handleMessage() (usbh thread, atomic increment only - same
    // contract as the rest of that function).
    std::atomic<uint32_t> ignoredLargeCommandCount_{0};

    // Guards fileStream_ and the five *AtStart_ snapshots below: all are
    // written from runHandshake() at stream start, read/diffed from
    // runHeartbeat() at stream finish, and fileStream_ is also written from
    // onMessage()/onInflateFailed() (the dispatcher_/gzipInflater_ worker
    // threads) - never from `usbh`.
    struct k_mutex fileStreamMutex_ {};
    QcFileStream fileStream_;
    uint32_t dispatcherDroppedAtStart_ = 0;
    uint32_t gzipDroppedAtStart_ = 0;
    uint32_t reassemblyOverflowAtStart_ = 0;
    uint32_t reassemblyMalformedAtStart_ = 0;
    uint32_t totalInTransfersAtStart_ = 0;

    // checkTrafficHealth()'s own "did anything change since last time"
    // state - read and written only from the heartbeat thread, so no lock
    // needed.
    uint32_t lastReportedDispatcherDropped_ = 0;
    uint32_t lastReportedGzipDropped_ = 0;
    uint32_t lastReportedReassemblyOverflow_ = 0;
    uint32_t lastReportedReassemblyMalformed_ = 0;

    // A new command id is always logged the moment it's first seen; a
    // count bump on an already-logged command is throttled to at most
    // once per this interval (see runHeartbeat()), and even then only that
    // command's line is reprinted, not the whole tracked set. 60s is long
    // enough that continuous streaming traffic (confirmed on hardware,
    // commit 0d1edcc: GlobalTempo at ~4/s for the whole session) can't
    // flood the log, while still confirming the session is alive
    // periodically rather than going silent forever after the first line.
    static constexpr int64_t kPostStreamLogIntervalMs = 60000;

    // issue #61: everything postStreamTally_ needs to answer "did a
    // preset change on the unit push something new (RecallPreset/Scene/
    // SetlistPosition/...)" without re-logging unchanged counts every
    // ~200ms heartbeat tick. Guarded by fileStreamMutex_ above, same as
    // fileStream_ - started in runHeartbeat() the moment the File stream's
    // summary is produced, and never stopped again (unlike fileStream_,
    // which is scoped to one File stream).
    QcMessageTally postStreamTally_;
    std::array<QcMessageTally::Entry, QcMessageTally::kMaxDistinctCommands> postStreamLoggedEntries_{};
    size_t postStreamLoggedEntryCount_ = 0;
    uint32_t postStreamLoggedOtherCount_ = 0;
    int64_t postStreamLastLoggedAtMs_ = 0;
};

}  // namespace qcbridge
