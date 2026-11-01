#pragma once

#include <cstddef>

#include <etl/vector.h>

#include "QcPresetCatalog.hpp"

namespace qcbridge {

// A self-contained copy of QcPresetCatalog.hpp's contents (issue #71) - the
// thing a transport (QcPresetTransport.hpp) actually hands to a client, so
// that neither QcSession nor any transport implementation needs to touch
// QcPresetCatalog's own storage or locking directly. Same reasoning as
// QcPresetPrinter.hpp's FolderSnapshot, generalized to the whole catalog at
// once rather than one folder at a time: a transport publishing JSON wants
// the full listing in one shot, not a per-folder callback sequence.
//
// Plain C++23, no Zephyr/Wi-Fi/HTTP dependency - directly unit-testable
// (see tests/preset_transport). copyCatalogSnapshot() is the only thing
// that needs QcPresetCatalog's own mutex held around it (same contract as
// QcPresetPrinter::printCatalog()'s copy step) - the snapshot itself is
// read-only afterwards.
struct QcCatalogSnapshot {
    struct PresetEntry {
        uint32_t index = 0;
        QcPresetCatalog::NameString name;
    };

    struct FolderEntry {
        QcPresetCatalog::NameString name;
        etl::vector<PresetEntry, QcPresetCatalog::kMaxPresetsPerFolder> presets;
    };

    etl::vector<FolderEntry, QcPresetCatalog::kMaxFolders> folders;
    size_t presetCount = 0;
    bool incomplete = false;
    uint32_t generation = 0;
};

// Copies every folder/preset out of `catalog` into `out` - call while
// holding the same mutex QcSession guards every catalog write with (see
// QcPresetPrinter.hpp's own comment on why). `out` is sized to mirror
// QcPresetCatalog's own capacities, so this never truncates silently.
inline void copyCatalogSnapshot(const QcPresetCatalog& catalog, bool incomplete,
                                 QcCatalogSnapshot& out) {
    out.folders.clear();
    out.presetCount = catalog.presetCount();
    out.incomplete = incomplete;
    out.generation = catalog.generation();

    for (const auto& folder : catalog.folders()) {
        out.folders.emplace_back();
        auto& folderOut = out.folders.back();
        folderOut.name = folder.name;
        for (const auto& preset : folder.presets) {
            folderOut.presets.emplace_back();
            auto& presetOut = folderOut.presets.back();
            presetOut.index = preset.index;
            presetOut.name = preset.name;
        }
    }
}

}  // namespace qcbridge
