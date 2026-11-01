#pragma once

#include <atomic>
#include <cstdint>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "QcPresetCatalog.hpp"

namespace qcbridge {

// Prints QcPresetCatalog.hpp's listing once per finished File-read stream
// (issue #10's MVP1 acceptance criterion), on its own thread rather than
// the heartbeat thread that detects the stream's finish.
//
// Why a dedicated thread: CONFIG_LOG_MODE_IMMEDIATE (prj.conf) makes every
// LOG_INF call block the calling thread until the UART write completes.
// QcSession's own catalog can hold several dozen presets (ADR 0007's
// hardware run: 48 named across 3 folders) - one LOG_INF line per preset
// plus a header per folder, done in a loop, adds up to real wall-clock
// time. Doing that on `qc_heartbeat` itself would delay that thread's own
// next ~200ms KeepAlive send by however long the whole listing takes to
// print - exactly what issue #8/#33 build the heartbeat's priority
// (QcSession::kHeartbeatThreadPriority) to avoid. Printing from a separate,
// *lower*-priority thread means Zephyr's preemptive scheduler always lets
// `qc_heartbeat` run (and send its KeepAlive) the moment it's ready,
// regardless of how long this thread is mid-LOG_INF - the two threads don't
// share a lock either (see below), so there's nothing here that could make
// `qc_heartbeat` wait on this thread.
//
// Thread-safety: requestPrint() is called from `qc_heartbeat` once a
// QcFileStream summary is produced - at that point QcPresetCatalog is
// guaranteed to receive no further writes until the next reset() (see
// QcPresetCatalog.hpp's own comment), so this thread reads the catalog
// with no lock of its own, just the generation check below to notice a
// reconnect's reset() racing an in-progress print and stop early rather
// than print a mix of two listings.
class QcPresetPrinter {
public:
    QcPresetPrinter() { k_sem_init(&printRequestSem_, 0, 1); }

    // Lazily starts the worker thread - same idempotent pattern as
    // QcDispatcher::start()/QcGzipInflater::start().
    void start() {
        if (threadStarted_) {
            return;
        }
        k_thread_create(&thread_, stack_, K_KERNEL_STACK_SIZEOF(stack_), &threadEntryTrampoline, this,
                        nullptr, nullptr, kThreadPriority, 0, K_NO_WAIT);
        k_thread_name_set(&thread_, "qc_preset_print");
        threadStarted_ = true;
    }

    // Called from `qc_heartbeat` right after a File stream's summary is
    // logged. `incomplete` says whether the summary showed any
    // overflow/dropped/failed File messages, or this catalog itself ran
    // out of room (QcPresetCatalog::overflowed()) - either way the
    // printed listing should say so plainly (issue #10).
    void requestPrint(const QcPresetCatalog& catalog, bool incomplete) {
        pendingCatalog_ = &catalog;
        pendingGeneration_ = catalog.generation();
        pendingIncomplete_ = incomplete;
        k_sem_give(&printRequestSem_);
    }

private:
    // Below `qc_heartbeat` (QcSession::kHeartbeatThreadPriority, 5) and
    // every other named thread in this firmware - printing can always be
    // delayed without harming protocol timing, which is the whole point of
    // moving it off the heartbeat thread in the first place.
    static constexpr int kThreadPriority = 9;

    static void threadEntryTrampoline(void* p1, void*, void*) {
        static_cast<QcPresetPrinter*>(p1)->run();
    }

    void run() {
        while (true) {
            k_sem_take(&printRequestSem_, K_FOREVER);
            const QcPresetCatalog* catalog = pendingCatalog_;
            if (catalog != nullptr) {
                printCatalog(*catalog, pendingGeneration_, pendingIncomplete_);
            }
        }
    }

    void printCatalog(const QcPresetCatalog& catalog, uint32_t generation, bool incomplete) const {
        if (catalog.generation() != generation) {
            LOG_WRN("Preset catalog: listing reset before it could be printed");
            return;
        }

        const auto folders = catalog.folders();
        LOG_INF("Preset catalog: %zu folder(s), %zu preset(s) total%s", folders.size(),
                catalog.presetCount(),
                incomplete ? " - listing may be incomplete, see \"File stream limits\" above" : "");

        if (catalog.folderOverflowCount() > 0) {
            LOG_WRN("Preset catalog: %u folder(s) dropped - capacity (%zu) reached",
                    catalog.folderOverflowCount(), QcPresetCatalog::kMaxFolders);
        }
        if (catalog.presetOverflowCount() > 0) {
            LOG_WRN("Preset catalog: %u preset(s) dropped - a folder's capacity (%zu) was reached",
                    catalog.presetOverflowCount(), QcPresetCatalog::kMaxPresetsPerFolder);
        }

        for (const auto& folder : folders) {
            if (catalog.generation() != generation) {
                LOG_WRN("Preset catalog: listing reset mid-print, stopping early");
                return;
            }

            LOG_INF("Preset folder \"%s\" (%zu preset(s)):", folder.name.c_str(), folder.presets.size());
            for (const auto& preset : folder.presets) {
                LOG_INF("  [%u] %s", preset.index, preset.name.c_str());
            }
        }
    }

    K_KERNEL_STACK_MEMBER(stack_, 2048);
    struct k_thread thread_ {};
    bool threadStarted_ = false;
    struct k_sem printRequestSem_ {};

    const QcPresetCatalog* pendingCatalog_ = nullptr;
    uint32_t pendingGeneration_ = 0;
    bool pendingIncomplete_ = false;
};

}  // namespace qcbridge
