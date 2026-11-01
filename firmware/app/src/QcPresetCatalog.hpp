#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <etl/string.h>
#include <etl/vector.h>

#include "QcFileMessage.hpp"

namespace qcbridge {

// Holds the preset names decoded from the File-read stream (issues #59/#60,
// QcFileMessage.hpp's decodeFile()) so they survive past the call that
// produced them - decodeFile()'s FilePreset fields are std::string_view
// slices into QcSession's reassembly/inflate buffer, which gets reused by
// the very next message (see ADR 0006's "no bound on preset name length...
// a downstream cap belongs wherever the data is finally stored" note - this
// is that downstream cap). No heap, per CODING_STANDARDS.md: every folder
// and preset slot is fixed capacity.
//
// Not thread-safe by itself - same contract as QcFileStream.hpp/
// QcMessageTally.hpp. QcSession populates this from addPreset() on its
// dispatcher worker thread (never `usbh`), gated by the same
// fileStream_.isActive() check (under fileStreamMutex_) that guards
// QcFileStream::onMessage() - so by the time a File stream's summary is
// produced, this catalog is guaranteed quiescent until the next reset().
// generation() lets a reader (QcPresetPrinter.hpp) started after that point
// notice a later reset() (a reconnect racing an in-progress print) without
// needing a lock of its own.
class QcPresetCatalog {
public:
    // Real hardware (ADR 0007, commit 219cf6e, omit_factory_content=true)
    // found exactly 3 folders that ever hold a named preset on the owner's
    // unit (My Presets, Practice, cloud-0-1/"Downloads") - the other 12
    // folders in that run were plugin "User" folders with zero named
    // presets, which never register here at all (see addPreset() - a
    // folder only gets a slot once it has a *named* preset, which is
    // "leave out folders with no named presets" for free). 8 is a little
    // over 2.5x the observed 3, headroom for a unit with more cloud/plugin
    // folders that do hold named content, without trying to budget for
    // every folder the protocol could theoretically report.
    static constexpr size_t kMaxFolders = 8;

    // Observed max named presets in one folder on real hardware: 25
    // (cloud-0-1/Downloads); My Presets held 21. 64 is a little over 2.5x
    // the observed max - headroom for a bigger library without assuming
    // the fixed 256-slot table every folder physically has is ever close
    // to full of *named* entries (ADR 0007 sized the reassembly buffer for
    // that 256-named-slot worst case because a single oversized message
    // would otherwise be silently truncated mid-decode; this catalog
    // instead reports an explicit, counted overflow - see
    // presetOverflowCount() - so it doesn't need the same worst-case
    // assumption to stay safe).
    static constexpr size_t kMaxPresetsPerFolder = 64;

    // Longest real names seen (ADR 0007): "Salaatti-Classic" (16),
    // "SOAD Mezmerize" (14). 32 leaves real headroom for a longer
    // user-chosen name while keeping each slot's cost small - see this
    // class's RAM cost, reported in the PR that introduces it.
    static constexpr size_t kNameCapacity = 32;

    using NameString = etl::string<kNameCapacity>;

    struct PresetEntry {
        // The preset's real position within the folder's `files` array
        // (decodeFile()'s FilePreset::index) - not reassigned here, so a
        // folder with gaps (unused slots skipped, see addPreset()) still
        // reports each preset's true slot.
        uint32_t index = 0;
        NameString name;
    };

    struct Folder {
        NameString key;
        NameString name;
        etl::vector<PresetEntry, kMaxPresetsPerFolder> presets;
    };

    // Clears every folder/preset and bumps generation() - call when a new
    // File-read stream starts (QcSession::runHandshake(), the same point
    // that calls QcFileStream::start()) or the device reconnects. Covers
    // both cases with one call site: a reconnect always runs
    // runHandshake() again before issuing a new File READ.
    void reset() {
        folderCount_ = 0;
        presetCount_ = 0;
        folderOverflowCount_ = 0;
        presetOverflowCount_ = 0;
        for (auto& folder : folders_) {
            folder.presets.clear();
        }
        generation_.fetch_add(1, std::memory_order_relaxed);
    }

    // One call per decodeFile() preset slot (named or not) - this is where
    // "empty-named entries are unused slots" (issue #10) gets enforced, not
    // in the caller. A folder is registered on its first named preset, so a
    // folder that never has one (e.g. an empty plugin "User" folder) never
    // appears in folders() at all.
    void addPreset(const FilePreset& preset) {
        if (preset.presetName.empty()) {
            return;
        }

        Folder* folder = findFolder(preset.folderKey);
        if (folder == nullptr) {
            folder = addFolder(preset);
            if (folder == nullptr) {
                return;
            }
        }

        if (folder->presets.full()) {
            ++presetOverflowCount_;
            return;
        }

        folder->presets.emplace_back();
        auto& entry = folder->presets.back();
        entry.index = preset.index;
        entry.name.assign(preset.presetName.data(), preset.presetName.size());
        ++presetCount_;
    }

    std::span<const Folder> folders() const { return {folders_.data(), folderCount_}; }
    size_t presetCount() const { return presetCount_; }

    // Non-zero means this catalog itself ran out of room (as opposed to the
    // device never sending everything - see QcFileStream::Summary for that)
    // - the listing printed from it is incomplete either way.
    uint32_t folderOverflowCount() const { return folderOverflowCount_; }
    uint32_t presetOverflowCount() const { return presetOverflowCount_; }
    bool overflowed() const { return folderOverflowCount_ > 0 || presetOverflowCount_ > 0; }

    // Bumped by reset() - a reader holding a generation seen before calling
    // this, after this call returns a different value, raced a reset() and
    // should stop trusting whatever it was reading.
    uint32_t generation() const { return generation_.load(std::memory_order_relaxed); }

private:
    // cloud-0-1 ships with an empty folder name on real hardware (ADR 0007)
    // - label it rather than printing a blank line. Any other folder with
    // an empty name (not expected on real hardware, but not ruled out by
    // the protocol either) falls back to its key instead of a blank line.
    static constexpr std::string_view kDownloadsFolderKey = "cloud-0-1";
    static constexpr std::string_view kDownloadsLabel = "Downloads";

    static std::string_view labelFor(const FilePreset& preset) {
        if (!preset.folderName.empty()) {
            return preset.folderName;
        }
        return preset.folderKey == kDownloadsFolderKey ? kDownloadsLabel : preset.folderKey;
    }

    Folder* findFolder(std::string_view key) {
        for (size_t i = 0; i < folderCount_; ++i) {
            if (std::string_view(folders_[i].key.data(), folders_[i].key.size()) == key) {
                return &folders_[i];
            }
        }
        return nullptr;
    }

    Folder* addFolder(const FilePreset& preset) {
        if (folderCount_ >= folders_.size()) {
            ++folderOverflowCount_;
            return nullptr;
        }
        Folder& folder = folders_[folderCount_];
        folder.presets.clear();
        folder.key.assign(preset.folderKey.data(), preset.folderKey.size());
        const std::string_view label = labelFor(preset);
        folder.name.assign(label.data(), label.size());
        ++folderCount_;
        return &folder;
    }

    std::array<Folder, kMaxFolders> folders_{};
    size_t folderCount_ = 0;
    size_t presetCount_ = 0;
    uint32_t folderOverflowCount_ = 0;
    uint32_t presetOverflowCount_ = 0;
    std::atomic<uint32_t> generation_{0};
};

}  // namespace qcbridge
