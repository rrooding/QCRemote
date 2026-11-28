#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

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
        // The Preset's index across the whole Catalog, in listing order
        // (docs/api) - not QcPresetCatalog::PresetEntry::index, its slot
        // within its Folder.
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

    uint32_t catalogIndex = 0;
    for (const auto& folder : catalog.folders()) {
        out.folders.emplace_back();
        auto& folderOut = out.folders.back();
        folderOut.name = folder.name;
        for (const auto& preset : folder.presets) {
            folderOut.presets.emplace_back();
            auto& presetOut = folderOut.presets.back();
            presetOut.index = catalogIndex++;
            presetOut.name = preset.name;
        }
    }
}

// The Catalog entry at `slot` of the Folder keyed `folderKey` - how the
// Quad Cortex names its Current preset (SetlistPosition's folder key and
// position) - with the same Catalog-wide index copyCatalogSnapshot() gives
// it. Empty when that slot holds no named Preset in this Catalog. Same
// locking contract as copyCatalogSnapshot().
inline std::optional<QcCatalogSnapshot::PresetEntry> findCurrentPreset(const QcPresetCatalog& catalog,
                                                                         std::string_view folderKey,
                                                                         uint32_t slot) {
    uint32_t catalogIndex = 0;
    for (const auto& folder : catalog.folders()) {
        const bool isFolder = folder.isKey(folderKey);
        for (const auto& preset : folder.presets) {
            if (isFolder && preset.index == slot) {
                QcCatalogSnapshot::PresetEntry entry;
                entry.index = catalogIndex;
                entry.name = preset.name;
                return entry;
            }
            ++catalogIndex;
        }
    }
    return std::nullopt;
}

// Where the Quad Cortex keeps a Preset: the Folder key and slot a
// SetlistPosition UPDATE loads it by (issue #78).
struct QcPresetLocation {
    // A view into the catalog's storage - copy it before releasing the lock.
    std::string_view folderKey;
    uint32_t slot = 0;
};

// The inverse of findCurrentPreset(): the Preset at Catalog-wide `index`.
// Empty past the end of the Catalog, or when its Folder's key was too long
// to keep whole and so can't be named back to the Quad Cortex. Same locking
// contract as copyCatalogSnapshot().
inline std::optional<QcPresetLocation> findPresetLocation(const QcPresetCatalog& catalog, uint32_t index) {
    uint32_t catalogIndex = 0;
    for (const auto& folder : catalog.folders()) {
        if (index - catalogIndex < folder.presets.size()) {
            if (!folder.keyIsWhole()) {
                return std::nullopt;
            }
            return QcPresetLocation{.folderKey = std::string_view(folder.key.data(), folder.key.size()),
                                    .slot = folder.presets[index - catalogIndex].index};
        }
        catalogIndex += static_cast<uint32_t>(folder.presets.size());
    }
    return std::nullopt;
}

}  // namespace qcbridge
