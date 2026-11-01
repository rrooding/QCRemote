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
#include "QcFileReadExperiment.hpp"
#include "QcFileRequest.hpp"
#include "QcFileStream.hpp"
#include "QcGzip.hpp"
#include "QcGzipInflater.hpp"
#include "QcHandshakeMessages.hpp"
#include "QcMessage.hpp"
#include "QcMessageSink.hpp"
#include "QcMessageTally.hpp"
#include "QcReassemblyOverflowTally.hpp"
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
        }

        // messageTally_ records every command, not just File - see its own
        // header comment (issue #60's hardware debugging: a run with zero
        // File messages couldn't otherwise tell "the device sent nothing"
        // apart from "the device sent something else"). wireSize/
        // inflatedSize feed its per-command peak-size tracking (issue #33).
        ScopedKMutex lock(fileStreamMutex_);
        messageTally_.record(command, wireSize, inflatedSize);
        postStreamTally_.record(command, wireSize, inflatedSize);
        if (command == kCommandFile) {
            fileStream_.onMessage(k_uptime_get(), wireSize, inflatedSize, compressed);

            // issue #33 variant experiment: decode this message's content
            // right here, on this same worker thread (never usbh - see
            // handleMessage()'s comment, and QcFileReadExperiment.hpp's own
            // comment on why this is stack-safe). `protoBytes` is only
            // valid for a message that fit (reached onMessage() at all);
            // onInflateFailed() below has no decoded bytes to offer it.
            fileReadExperiment_.onFileMessage(
                protoBytes, [](std::string_view folderName, std::string_view presetName) {
                    LOG_INF("File content sample: folder=\"%.*s\" preset=\"%.*s\"",
                            static_cast<int>(folderName.size()), folderName.data(),
                            static_cast<int>(presetName.size()), presetName.data());
                });
        }
    }

    void onInflateFailed(uint16_t command, size_t wireSize, const char* reason,
                          std::optional<size_t> trueInflatedSize) override {
        if (command == kCommandFile) {
            LOG_WRN("File message failed to inflate (wire=%zu bytes): %s", wireSize, reason);
        }

        ScopedKMutex lock(fileStreamMutex_);
        messageTally_.record(command, wireSize, trueInflatedSize.value_or(0));
        postStreamTally_.record(command, wireSize, trueInflatedSize.value_or(0));
        if (command == kCommandFile) {
            fileStream_.onInflateFailed(k_uptime_get(), wireSize, trueInflatedSize);
        }
    }

    // Called from QcHidBridge::onReportIn() on the `usbh` thread for
    // HidReassembler::Status::Overflow (issue #60's hardware-safety
    // follow-up, extended by issue #33) - atomic increment only, same
    // contract as handleMessage() above: no logging, nothing that can
    // block. The plain lifetime count (reassemblyOverflowCount_) can't be
    // attributed to the File stream specifically by itself - see
    // QcFileStream::Summary's comment on reassemblyOverflowCount for why
    // it's still folded into that summary anyway, as a best-effort "this
    // happened somewhere during the stream" figure. `command` (decoded from
    // the message's surviving trailer - see QcHidBridge.hpp's Overflow
    // case) and `trueSize` additionally feed reassemblyOverflowByCommand_,
    // which is what #33's sizing exercise actually needs: not just how
    // often this happens, but which command(s) and how big they really are.
    void recordReassemblyOverflow(uint16_t command, size_t trueSize) {
        reassemblyOverflowCount_.fetch_add(1, std::memory_order_relaxed);
        reassemblyOverflowByCommand_.record(command, trueSize);
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
        // message (QcDispatcher.hpp/QcGzipInflater.hpp), so a large or
        // compressed ModelRepo reply that overflows kReassemblyCapacity or
        // either mailbox is safely dropped and counted there, not crashed.
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
        // issue #33 variant experiment: instead of always the same bare
        // READ, this cycles through QcFileReadExperiment::kVariants - one
        // at a time, each to its own stream finish (runHeartbeat() sends
        // the next one once the current one goes quiet/times out) - to
        // test whether short_listing/omit_factory_content shrink the
        // listing. Variant 0 ("default") has both options false, which
        // encodeFileRead() always omits entirely, so it reproduces the
        // original bare File READ byte-for-byte - this must match the
        // a1c25b6 hardware numbers quoted in firmware/README.md's issue
        // #33 section.
        FileReadOptions variantOptions;
        std::string_view variantLabel;
        size_t variantNumber = 0;
        {
            ScopedKMutex lock(fileStreamMutex_);
            fileReadExperiment_.reset();
            fileReadExperiment_.beginVariant();
            reassemblyOverflowByCommand_.reset();
            fileStream_.start(k_uptime_get());
            messageTally_.start();
            dispatcherDroppedAtStart_ = dispatcher_.droppedCount();
            gzipDroppedAtStart_ = gzipInflater_.droppedCount();
            reassemblyOverflowAtStart_ = reassemblyOverflowCount_.load(std::memory_order_relaxed);
            reassemblyMalformedAtStart_ = reassemblyMalformedCount_.load(std::memory_order_relaxed);
            totalInTransfersAtStart_ = totalInTransfers_.load(std::memory_order_relaxed);
            variantOptions = fileReadExperiment_.currentVariant().options;
            variantLabel = fileReadExperiment_.currentVariant().label;
            variantNumber = fileReadExperiment_.variantNumber();
        }
        msg.clear();
        encodeFileRead(msg, nextRequestId(), variantOptions);
        LOG_INF("File READ variant experiment: sending variant %zu/%zu (%.*s)", variantNumber,
                QcFileReadExperiment::kVariants.size(), static_cast<int>(variantLabel.size()),
                variantLabel.data());
        if (!sendMessage(msg)) {
            LOG_ERR("Handshake: failed to send File READ (variant=%.*s)",
                    static_cast<int>(variantLabel.size()), variantLabel.data());
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
            std::array<QcMessageTally::Entry, QcMessageTally::kMaxDistinctCommands> tallyEntries{};
            size_t tallyEntryCount = 0;
            uint32_t tallyOtherCount = 0;
            uint32_t totalInTransfersThisStream = 0;
            bool postStreamChanged = false;
            std::array<QcMessageTally::Entry, QcMessageTally::kMaxDistinctCommands> postStreamEntries{};
            size_t postStreamEntryCount = 0;
            uint32_t postStreamOtherCount = 0;
            // issue #33 variant experiment: which variant this summary (if
            // any) belongs to, its content-decode counters, and whether it
            // was the last variant - captured here, before anything below
            // resets them for the next one.
            std::string_view finishedVariantLabel;
            QcFileReadExperiment::ContentSummary contentSummary{};
            bool experimentDone = false;
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

                    // Copy out before stop() so the log lines below don't
                    // need the lock - see QcMessageTally.hpp.
                    const auto entries = messageTally_.entries();
                    tallyEntryCount = entries.size();
                    for (size_t i = 0; i < tallyEntryCount; ++i) {
                        tallyEntries[i] = entries[i];
                    }
                    tallyOtherCount = messageTally_.otherCount();
                    messageTally_.stop();

                    // issue #33 variant experiment: same "copy out before
                    // anything resets it" reasoning as messageTally_ above.
                    finishedVariantLabel = fileReadExperiment_.currentVariant().label;
                    contentSummary = fileReadExperiment_.contentSummary();
                    experimentDone = fileReadExperiment_.isLastVariant();

                    // Only start watching for post-connect preset-change
                    // pushes (issue #61) once every variant has had its own
                    // turn - the code below (outside this lock) sends the
                    // next variant's File READ otherwise, which must not be
                    // mistaken by postStreamTally_ for a preset change.
                    if (experimentDone) {
                        fileReadExperiment_.advanceVariant();

                        // issue #61's preset-change check needs visibility
                        // into whatever arrives *after* the File stream,
                        // too (a preset recall on the unit pushing
                        // RecallPreset/Scene/SetlistPosition or similar) -
                        // messageTally_ above is scoped to the stream and
                        // goes silent the moment it stops. postStreamTally_
                        // picks up right here and keeps running for the
                        // rest of the session; see the "changed since last
                        // logged" check below for how it's reported without
                        // flooding the log if the device turns out to
                        // stream something continuously (qc-mcp's own
                        // comment: GlobalTempo/CPULoad stream ~3/s under an
                        // active heartbeat, with or without a preset
                        // change).
                        postStreamTally_.start();
                        postStreamLoggedEntries_ = {};
                        postStreamLoggedEntryCount_ = 0;
                        postStreamLoggedOtherCount_ = 0;
                        postStreamLastLoggedAtMs_ = k_uptime_get();
                    }
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
                // Moved to its own function deliberately (not inlined here)
                // - a real hardware run crashed qc_heartbeat with a stack
                // overflow (USAGE FAULT, "context area not valid") on its
                // very first tick, before any File stream had even started,
                // when this logging/next-variant-send logic was inline:
                // every local declared anywhere in runHeartbeat()'s body
                // (including inside this "if (summary)" block, almost never
                // taken) is part of its one stack frame, reserved on every
                // single tick regardless of which branch runs. Calling a
                // separate function instead means those locals (the
                // overflow-by-command array, the next variant's send
                // buffer, ...) only ever cost stack while this function is
                // actually executing - a few times total across the whole
                // experiment, not every ~200ms tick.
                logFileStreamSummaryAndAdvance(*summary, {tallyEntries.data(), tallyEntryCount},
                                               tallyOtherCount, totalInTransfersThisStream,
                                               finishedVariantLabel, contentSummary, experimentDone);
            }
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

    // issue #33 variant experiment: logs one variant's finished-stream
    // summary and either sends the next variant's File READ or (on the
    // last variant) hands off to issue #61's post-connect watch. Called
    // from runHeartbeat() only when a stream just finished - see that call
    // site's comment for why this is a separate function rather than
    // inlined there (a real hardware stack overflow on qc_heartbeat).
    // `tallyEntries` points into runHeartbeat()'s own already-copied-out
    // array, valid for the duration of this call.
    void logFileStreamSummaryAndAdvance(const QcFileStream::Summary& summary,
                                        std::span<const QcMessageTally::Entry> tallyEntries,
                                        uint32_t tallyOtherCount, uint32_t totalInTransfersThisStream,
                                        std::string_view variantLabel,
                                        const QcFileReadExperiment::ContentSummary& contentSummary,
                                        bool experimentDone) {
        const int labelLen = static_cast<int>(variantLabel.size());
        const char* const label = variantLabel.data();
        LOG_INF("File stream [variant=%.*s] finished (%s): %u messages (%u compressed, %u "
                "failed), %zu wire bytes, %zu inflated bytes, %d ms",
                labelLen, label, summary.timedOut ? "timeout" : "quiet", summary.messageCount,
                summary.compressedCount, summary.failedCount, summary.totalWireBytes,
                summary.totalInflatedBytes, static_cast<int>(summary.elapsedMs));
        // Separate line: sizing (#33) and data-loss figures. A non-zero
        // dropped/overflow/malformed count here is the known gap (see
        // firmware/README.md's issue #60 section) - not necessarily
        // specific to File messages for the reassembly counters (they're
        // counted before a command id is known), but real evidence of loss
        // during this variant's window either way.
        LOG_INF("File stream [variant=%.*s] limits: max wire=%zu max inflated=%zu, dropped "
                "(dispatch=%u gzip=%u), reassembly (overflow=%u malformed=%u)",
                labelLen, label, summary.maxWireBytes, summary.maxInflatedBytes,
                summary.dispatcherDropped, summary.gzipDropped, summary.reassemblyOverflowCount,
                summary.reassemblyMalformedCount);
        // Diagnostic (issue #60's hardware debugging): every command
        // actually seen during the window, not just File - disambiguates
        // "the device sent nothing" from "the device sent something, just
        // not File". totalInTransfersThisStream goes one layer lower
        // still: it counts successful USB IN transfers regardless of
        // whether the reassembler ever turned them into a counted message,
        // so a run with 0 messages of every kind above but a nonzero
        // transfer count here means the device sent *something* that never
        // made it past HidReassembler (overflow/malformed should also be
        // nonzero in that case) - versus 0 transfers, which means nothing
        // arrived on the wire at all.
        LOG_INF("File stream [variant=%.*s] tally: %u total IN transfers", labelLen, label,
                totalInTransfersThisStream);
        if (tallyEntries.empty() && tallyOtherCount == 0) {
            LOG_INF("File stream [variant=%.*s] tally: no messages of any command seen in this "
                    "window",
                    labelLen, label);
        } else {
            for (const auto& entry : tallyEntries) {
                // max_wire/max_inflated (issue #33): the peak sizes this
                // specific command produced among messages that fit -
                // QcFileStream's own maxWireBytes/maxInflatedBytes above
                // are the peak across every command, this is the
                // per-command breakdown #33's sizing exercise needs.
                LOG_INF("File stream [variant=%.*s] tally: command=%u count=%u max_wire=%zu "
                        "max_inflated=%zu",
                        labelLen, label, entry.command, entry.count, entry.maxWireBytes,
                        entry.maxInflatedBytes);
            }
            if (tallyOtherCount > 0) {
                LOG_INF("File stream [variant=%.*s] tally: %u messages from other distinct "
                        "commands (> %zu tracked)",
                        labelLen, label, tallyOtherCount, QcMessageTally::kMaxDistinctCommands);
            }
        }

        // issue #33 variant experiment: which command(s) actually
        // overflowed kReassemblyCapacity during *this* variant, and how
        // big they really were. reassemblyOverflowByCommand_ is reset
        // right before each variant's File READ is sent (see
        // runHandshake() and the next-variant block below), so unlike the
        // original #33 measurement's single run, these numbers are scoped
        // to this variant, not lifetime-since-boot.
        std::array<QcReassemblyOverflowTally::Entry, QcReassemblyOverflowTally::kMaxDistinctCommands>
            overflowEntries{};
        const size_t overflowEntryCount = reassemblyOverflowByCommand_.entries(overflowEntries);
        const uint32_t overflowOtherCount = reassemblyOverflowByCommand_.otherCount();
        if (overflowEntryCount == 0 && overflowOtherCount == 0) {
            LOG_INF("File stream [variant=%.*s] overflow by command: none", labelLen, label);
        } else {
            for (size_t i = 0; i < overflowEntryCount; ++i) {
                LOG_INF("File stream [variant=%.*s] overflow by command: command=%u count=%u "
                        "max_true_size=%u",
                        labelLen, label, overflowEntries[i].command, overflowEntries[i].count,
                        overflowEntries[i].maxTrueSize);
            }
            if (overflowOtherCount > 0) {
                LOG_INF("File stream [variant=%.*s] overflow by command: %u overflow(s) from "
                        "other distinct commands (> %zu tracked), largest true size %u",
                        labelLen, label, overflowOtherCount,
                        QcReassemblyOverflowTally::kMaxDistinctCommands,
                        reassemblyOverflowByCommand_.otherMaxTrueSize());
            }
        }

        // issue #33 variant experiment: content actually decoded from this
        // variant's messages that fit (QcFileMessage.hpp's decodeFile(),
        // called from onMessage() as each message arrived - see
        // QcFileReadExperiment.hpp). A handful of folder/preset name
        // samples were already logged live, during the stream, at a
        // bounded low rate - not batched here, to keep this a summary
        // rather than a second per-message log.
        LOG_INF("File content [variant=%.*s]: preset folders=%u presets=%u empty-named=%u", labelLen,
                label, contentSummary.presetFolderCount, contentSummary.totalPresetCount,
                contentSummary.emptyNamePresetCount);
        if (contentSummary.typeCountEntries == 0 && contentSummary.otherTypeCount == 0) {
            LOG_INF("File content [variant=%.*s]: no File messages of any type seen this variant",
                    labelLen, label);
        } else {
            // decodeFile() only ever reports presets (type 0) - this is
            // every File message's `type` field, independent of
            // decodeFile(), so a message dropped from the decoded preset
            // count above (e.g. an IRs/Captures folder) still shows up
            // here.
            for (size_t i = 0; i < contentSummary.typeCountEntries; ++i) {
                LOG_INF("File content [variant=%.*s]: type=%d count=%u", labelLen, label,
                        static_cast<int>(contentSummary.typeCounts[i].type),
                        contentSummary.typeCounts[i].count);
            }
            if (contentSummary.otherTypeCount > 0) {
                LOG_INF("File content [variant=%.*s]: %u messages from other distinct types "
                        "(> %zu tracked)",
                        labelLen, label, contentSummary.otherTypeCount,
                        QcFileReadExperiment::kMaxTypeEntries);
            }
        }

        if (experimentDone) {
            LOG_INF("Post-connect tally started: watching for preset-change pushes "
                    "(RecallPreset/Scene/SetlistPosition/...) - issue #61");
            return;
        }

        // issue #33 variant experiment: move on to the next variant -
        // reusing the same send path the heartbeat already uses for
        // KeepAlive, rather than a new thread.
        FileReadOptions nextOptions;
        std::string_view nextLabel;
        size_t nextVariantNumber = 0;
        {
            ScopedKMutex lock(fileStreamMutex_);
            fileReadExperiment_.advanceVariant();
            fileReadExperiment_.beginVariant();
            reassemblyOverflowByCommand_.reset();
            fileStream_.start(k_uptime_get());
            messageTally_.start();
            dispatcherDroppedAtStart_ = dispatcher_.droppedCount();
            gzipDroppedAtStart_ = gzipInflater_.droppedCount();
            reassemblyOverflowAtStart_ = reassemblyOverflowCount_.load(std::memory_order_relaxed);
            reassemblyMalformedAtStart_ = reassemblyMalformedCount_.load(std::memory_order_relaxed);
            totalInTransfersAtStart_ = totalInTransfers_.load(std::memory_order_relaxed);
            nextOptions = fileReadExperiment_.currentVariant().options;
            nextLabel = fileReadExperiment_.currentVariant().label;
            nextVariantNumber = fileReadExperiment_.variantNumber();
        }
        HandshakeMessageBuffer fileReadMsg;
        encodeFileRead(fileReadMsg, nextRequestId(), nextOptions);
        LOG_INF("File READ variant experiment: sending variant %zu/%zu (%.*s)", nextVariantNumber,
                QcFileReadExperiment::kVariants.size(), static_cast<int>(nextLabel.size()),
                nextLabel.data());
        if (!sendMessage(fileReadMsg)) {
            LOG_ERR("File READ variant experiment: failed to send variant (%.*s)",
                    static_cast<int>(nextLabel.size()), nextLabel.data());
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

    // 2048 was enough pre-#33-variant-experiment (measured peak 1052/2048
    // on commit a1c25b6). This thread now also encodes and sends each next
    // variant's File READ (logFileStreamSummaryAndAdvance() -> sendMessage())
    // - a real hardware run crashed here with a stack overflow before that
    // was accounted for (see logFileStreamSummaryAndAdvance()'s comment).
    // Doubled for margin rather than hand-counting bytes exactly, since
    // this can't be re-verified on hardware from here; revert alongside
    // the rest of the experiment once #33 picks real sizes.
    K_KERNEL_STACK_MEMBER(heartbeatStack_, 4096);
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

    // Per-command breakdown of reassemblyOverflowCount_ above (issue #33) -
    // own lock-free contract, see QcReassemblyOverflowTally.hpp.
    QcReassemblyOverflowTally reassemblyOverflowByCommand_;

    // Written from QcHidBridge::onReportIn() on the `usbh` thread (atomic
    // increment only, same contract as the two above) - one per successful
    // IN transfer, independent of the reassembler's verdict on it.
    std::atomic<uint32_t> totalInTransfers_{0};

    // Guards fileStream_, messageTally_ and the five *AtStart_ snapshots
    // below: all are written from runHandshake() at stream start, read/
    // diffed from runHeartbeat() at stream finish, and fileStream_/
    // messageTally_ are also written from onMessage()/onInflateFailed()
    // (the dispatcher_/gzipInflater_ worker threads) - never from `usbh`.
    struct k_mutex fileStreamMutex_ {};
    QcFileStream fileStream_;
    QcMessageTally messageTally_;

    // issue #33 variant experiment (QcFileReadExperiment.hpp) - guarded by
    // fileStreamMutex_ same as the two members above: beginVariant()/reset()
    // from runHandshake()/runHeartbeat(), onFileMessage() from onMessage().
    // Self-contained; delete this member and its call sites (grep "issue
    // #33 variant experiment") to remove the whole experiment.
    QcFileReadExperiment fileReadExperiment_;
    uint32_t dispatcherDroppedAtStart_ = 0;
    uint32_t gzipDroppedAtStart_ = 0;
    uint32_t reassemblyOverflowAtStart_ = 0;
    uint32_t reassemblyMalformedAtStart_ = 0;
    uint32_t totalInTransfersAtStart_ = 0;

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
    // messageTally_ - started in runHeartbeat() the moment the File
    // stream's summary is produced, and never stopped again (unlike
    // messageTally_, which is scoped to one File stream).
    QcMessageTally postStreamTally_;
    std::array<QcMessageTally::Entry, QcMessageTally::kMaxDistinctCommands> postStreamLoggedEntries_{};
    size_t postStreamLoggedEntryCount_ = 0;
    uint32_t postStreamLoggedOtherCount_ = 0;
    int64_t postStreamLastLoggedAtMs_ = 0;
};

}  // namespace qcbridge
