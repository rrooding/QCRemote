#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "QcPresetCatalog.hpp"
#include "ScopedKMutex.hpp"

namespace qcbridge {

namespace detail {
// Copies at most dest.size() entries from `source` into `dest`, returning
// how many were copied. Pure data movement, no locking of its own -
// QcPresetPrinter::printCatalog() is responsible for calling this while
// holding its catalog mutex (see that class's header comment). Pulled out
// as a free function so its only real failure mode (writing past `dest`'s
// bound) is unit-testable without a Zephyr thread/kernel fixture - see
// tests/preset_printer.
inline size_t copyPresetEntries(
    std::span<const QcPresetCatalog::PresetEntry> source,
    std::array<QcPresetCatalog::PresetEntry, QcPresetCatalog::kMaxPresetsPerFolder>& dest) {
    const size_t count = std::min(source.size(), dest.size());
    for (size_t i = 0; i < count; ++i) {
        dest[i] = source[i];
    }
    return count;
}
}  // namespace detail

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
// regardless of how long this thread is mid-LOG_INF.
//
// Thread-safety (issue #10 Copilot review, see ADR 0008): requestPrint()
// publishes a request (the catalog to print, its generation, and whether
// it's already known incomplete) under requestMutex_ as one atomic step,
// and run() reads it back the same way - a second request landing while
// this thread is still mid-k_sem_take() can only ever replace the whole
// tuple, never tear it into a mix of two requests. Printing itself
// (printCatalog()) copies one folder's worth of entries out of the catalog
// under catalogMutex_ - the *same* mutex QcSession holds for every write
// to the catalog - before logging them, so the slow LOG_INF/LOG_WRN calls
// that can run arbitrarily long never happen while a reconnect's reset()
// could be mutating the same storage. Both critical sections are
// deliberately tiny (an assignment, or a copy of a few dozen small
// structs) - never the printing itself - so this thread contending for
// catalogMutex_ can't meaningfully delay `qc_heartbeat`'s own brief use of
// it.
class QcPresetPrinter {
public:
    QcPresetPrinter() {
        k_sem_init(&printRequestSem_, 0, 1);
        k_mutex_init(&requestMutex_);
    }

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

    // Must be called once, before the first requestPrint() - same mutex
    // QcSession guards every write to the catalog with (fileStreamMutex_).
    // See this class's header comment and printCatalog() for why printing
    // needs it too.
    void setCatalogMutex(k_mutex& catalogMutex) { catalogMutex_ = &catalogMutex; }

    // Called from `qc_heartbeat` right after a File stream's summary is
    // logged. `incomplete` says whether the summary showed any
    // overflow/dropped/failed File messages, a 30s overall timeout, or
    // this catalog itself ran out of room (QcPresetCatalog::overflowed())
    // - either way the printed listing should say so plainly (issue #10;
    // see QcFileStream::isListingIncomplete()).
    void requestPrint(const QcPresetCatalog& catalog, bool incomplete) {
        {
            ScopedKMutex lock(requestMutex_);
            pendingRequest_.catalog = &catalog;
            pendingRequest_.generation = catalog.generation();
            pendingRequest_.incomplete = incomplete;
        }
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

    struct PendingRequest {
        const QcPresetCatalog* catalog = nullptr;
        uint32_t generation = 0;
        bool incomplete = false;
    };

    void run() {
        while (true) {
            k_sem_take(&printRequestSem_, K_FOREVER);
            PendingRequest request;
            {
                ScopedKMutex lock(requestMutex_);
                request = pendingRequest_;
            }
            if (request.catalog != nullptr) {
                printCatalog(*request.catalog, request.generation, request.incomplete);
            }
        }
    }

    void printCatalog(const QcPresetCatalog& catalog, uint32_t generation, bool incomplete) {
        size_t folderCount = 0;
        size_t totalPresetCount = 0;
        uint32_t folderOverflowCount = 0;
        uint32_t presetOverflowCount = 0;
        {
            ScopedKMutex lock(*catalogMutex_);
            if (catalog.generation() != generation) {
                LOG_WRN("Preset catalog: listing reset before it could be printed");
                return;
            }
            folderCount = catalog.folders().size();
            totalPresetCount = catalog.presetCount();
            folderOverflowCount = catalog.folderOverflowCount();
            presetOverflowCount = catalog.presetOverflowCount();
        }

        LOG_INF("Preset catalog: %zu folder(s), %zu preset(s) total%s", folderCount, totalPresetCount,
                incomplete ? " - listing may be incomplete, see \"File stream limits\" above" : "");

        if (folderOverflowCount > 0) {
            // issue #10 Copilot review: this counts every named preset
            // rejected because no folder slot was left to hold it, not
            // distinct rejected folders - a 9-preset 9th folder once this
            // catalog already holds kMaxFolders would log "9", not "1".
            // Worded accordingly (was "N folder(s) dropped").
            LOG_WRN("Preset catalog: %u preset(s) rejected - folder capacity (%zu) reached",
                    folderOverflowCount, QcPresetCatalog::kMaxFolders);
        }
        if (presetOverflowCount > 0) {
            LOG_WRN("Preset catalog: %u preset(s) dropped - a folder's capacity (%zu) was reached",
                    presetOverflowCount, QcPresetCatalog::kMaxPresetsPerFolder);
        }

        for (size_t i = 0; i < folderCount; ++i) {
            size_t presetCountInFolder = 0;
            {
                ScopedKMutex lock(*catalogMutex_);
                if (catalog.generation() != generation) {
                    LOG_WRN("Preset catalog: listing reset mid-print, stopping early");
                    return;
                }
                const auto folders = catalog.folders();
                if (i >= folders.size()) {
                    // Can't happen while generation matches (folders only
                    // shrink via reset(), which always bumps it) - defends
                    // against a future change to that invariant rather
                    // than one observed in practice.
                    return;
                }
                folderSnapshot_.name = folders[i].name;
                const std::span<const QcPresetCatalog::PresetEntry> presets(folders[i].presets.data(),
                                                                              folders[i].presets.size());
                presetCountInFolder = detail::copyPresetEntries(presets, folderSnapshot_.presets);
            }

            // Everything printed below this point came from
            // folderSnapshot_ - member storage copied out above, not a
            // live view into the catalog - so the slow LOG_INF calls can
            // run as long as they need to without holding catalogMutex_.
            LOG_INF("Preset folder \"%s\" (%zu preset(s)):", folderSnapshot_.name.c_str(),
                    presetCountInFolder);
            for (size_t j = 0; j < presetCountInFolder; ++j) {
                LOG_INF("  [%u] %s", folderSnapshot_.presets[j].index,
                        folderSnapshot_.presets[j].name.c_str());
            }
        }
    }

    K_KERNEL_STACK_MEMBER(stack_, 2048);
    struct k_thread thread_ {};
    bool threadStarted_ = false;
    struct k_sem printRequestSem_ {};

    struct k_mutex requestMutex_ {};
    PendingRequest pendingRequest_;

    // Set once via setCatalogMutex() before start()/requestPrint() are
    // ever called - see printCatalog()'s own comment for why printing
    // needs it.
    k_mutex* catalogMutex_ = nullptr;

    // Scratch storage for one folder's worth of entries, copied out of the
    // catalog under catalogMutex_ before printing (see printCatalog()) -
    // member storage, not a stack local: too big for this thread's 2048 B
    // stack, same "big objects never go on a thread's stack" discipline as
    // QcPresetCatalog itself (ADR 0008).
    struct FolderSnapshot {
        QcPresetCatalog::NameString name;
        std::array<QcPresetCatalog::PresetEntry, QcPresetCatalog::kMaxPresetsPerFolder> presets;
    };
    FolderSnapshot folderSnapshot_;
};

}  // namespace qcbridge
