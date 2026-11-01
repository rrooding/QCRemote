#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>

#include "QcFileMessage.hpp"
#include "QcPresetCatalog.hpp"
#include "QcProtobuf.hpp"

using namespace qcbridge;

ZTEST_SUITE(preset_catalog, NULL, NULL, NULL, NULL, NULL);

namespace {

template <typename Container>
std::string_view asStringView(const Container& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// One ProductData entry: field 3 = name, left empty for an unused slot -
// mirrors the fixed per-folder slot table real hardware reports (ADR 0007).
// 256 comfortably covers this suite's longest fixture name (200 'p's, for
// the truncation test) plus its tag/length overhead.
etl::vector<uint8_t, 256> buildProduct(std::string_view name) {
    etl::vector<uint8_t, 256> out;
    writeString(out, 3, name);
    return out;
}

// A FolderInfo (field 1 = key, 3 = name, 7 = files, repeated). `slots` gives
// each files[] entry's name in array order - an empty string is an unused
// slot, matching real hardware (issue #10's "empty-named entries are unused
// slots"). 2048 covers this suite's biggest fixture (the per-folder capacity
// test's kMaxPresetsPerFolder + 5 short names, a few hundred bytes) with
// ample margin.
etl::vector<uint8_t, 2048> buildFolder(std::string_view key, std::string_view name,
                                       const std::vector<std::string_view>& slots) {
    etl::vector<uint8_t, 2048> out;
    writeString(out, 1, key);
    writeString(out, 3, name);
    for (const auto& slotName : slots) {
        const auto product = buildProduct(slotName);
        writeString(out, 7, asStringView(product));
    }
    return out;
}

template <typename Container>
etl::vector<uint8_t, 2176> buildFileMessage(const Container& folder) {
    etl::vector<uint8_t, 2176> proto;
    writeUInt64(proto, 3, kFileTypePresets);
    writeString(proto, 4, asStringView(folder));
    return proto;
}

void decodeInto(QcPresetCatalog& catalog, std::span<const uint8_t> proto) {
    decodeFile(proto, [&](const FilePreset& preset) { catalog.addPreset(preset); });
}

// QcPresetCatalog is a few KB (8 folders x 64 presets x a 32-byte name,
// CODING_STANDARDS.md-sized for the dispatcher thread's own static/member
// storage) - too big for a Ztest thread's stack as a per-test local, same
// "big objects go in static storage, never a stack temporary" lesson this
// firmware's own hardware crashes already taught (see QcSession.hpp's
// generation()/reset() comments). One instance, reset() before every test.
QcPresetCatalog& freshCatalog() {
    static QcPresetCatalog catalog;
    catalog.reset();
    return catalog;
}

}  // namespace

// --- skipped empties / real positions -------------------------------------

ZTEST(preset_catalog, test_skips_empty_named_entries_but_keeps_real_position)
{
    // Slots: [0]="", [1]="Lead Tone", [2]="", [3]="Clean Rhythm", [4]="" -
    // mirrors a mostly-empty real folder (ADR 0007: 21 named of 256 slots).
    const auto folder =
        buildFolder("1_q", "My Presets", {"", "Lead Tone", "", "Clean Rhythm", ""});
    const auto proto = buildFileMessage(folder);

    QcPresetCatalog& catalog = freshCatalog();
    decodeInto(catalog, {proto.data(), proto.size()});

    zassert_equal(catalog.presetCount(), 2u, "expected only the two named presets");
    const auto folders = catalog.folders();
    zassert_equal(folders.size(), 1u, "expected one registered folder");
    zassert_equal(folders[0].presets.size(), 2u, "expected two stored presets");

    zassert_equal(folders[0].presets[0].index, 1u, "first preset keeps its real slot index");
    zassert_equal(folders[0].presets[0].name, "Lead Tone", "first preset name");
    zassert_equal(folders[0].presets[1].index, 3u, "second preset keeps its real slot index");
    zassert_equal(folders[0].presets[1].name, "Clean Rhythm", "second preset name");
}

// --- empty folders are left out --------------------------------------------

ZTEST(preset_catalog, test_folder_with_no_named_presets_is_left_out)
{
    // Every slot empty - a plugin "User" folder with no named presets
    // (ADR 0007's hardware run: 12 of these on the owner's unit).
    const auto emptyFolder = buildFolder("/opt/neuraldsp/plugin-user", "User", {"", "", ""});
    const auto proto = buildFileMessage(emptyFolder);

    QcPresetCatalog& catalog = freshCatalog();
    decodeInto(catalog, {proto.data(), proto.size()});

    zassert_equal(catalog.presetCount(), 0u, "expected no presets");
    zassert_true(catalog.folders().empty(), "expected the folder to never register at all");
}

ZTEST(preset_catalog, test_multiple_folders_are_grouped_independently)
{
    // Each folder/message pair is built and decoded in its own scope - only
    // one pair's buffers are ever alive at once, same reasoning as this
    // file's buffer-capacity comments above.
    QcPresetCatalog& catalog = freshCatalog();
    {
        const auto folder = buildFolder("1_q", "My Presets", {"Lead Tone"});
        const auto proto = buildFileMessage(folder);
        decodeInto(catalog, {proto.data(), proto.size()});
    }
    {
        const auto folder = buildFolder("2_q", "Practice", {"Practice 1", "Dark Jazz"});
        const auto proto = buildFileMessage(folder);
        decodeInto(catalog, {proto.data(), proto.size()});
    }
    {
        const auto folder = buildFolder("/opt/neuraldsp/plugin-user", "User", {""});
        const auto proto = buildFileMessage(folder);
        decodeInto(catalog, {proto.data(), proto.size()});
    }

    const auto folders = catalog.folders();
    zassert_equal(folders.size(), 2u, "expected only the two folders with named presets");
    zassert_equal(catalog.presetCount(), 3u, "expected three presets total");

    zassert_equal(folders[0].name, "My Presets", "first folder name");
    zassert_equal(folders[0].presets.size(), 1u, "first folder preset count");
    zassert_equal(folders[1].name, "Practice", "second folder name");
    zassert_equal(folders[1].presets.size(), 2u, "second folder preset count");
    zassert_equal(folders[1].presets[0].name, "Practice 1", "second folder first preset");
    zassert_equal(folders[1].presets[1].name, "Dark Jazz", "second folder second preset");
}

// --- the downloads folder's unnamed-folder label ---------------------------

ZTEST(preset_catalog, test_downloads_folder_empty_name_is_labeled)
{
    // cloud-0-1 ships with an empty folder name on real hardware (ADR 0007).
    const auto downloadsFolder = buildFolder("cloud-0-1", "", {"SOAD Mezmerize", "Mused"});
    const auto proto = buildFileMessage(downloadsFolder);

    QcPresetCatalog& catalog = freshCatalog();
    decodeInto(catalog, {proto.data(), proto.size()});

    const auto folders = catalog.folders();
    zassert_equal(folders.size(), 1u, "expected one folder");
    zassert_equal(folders[0].name, "Downloads", "cloud-0-1's empty name is labeled Downloads");
    zassert_equal(folders[0].presets.size(), 2u, "expected both presets");
}

ZTEST(preset_catalog, test_other_empty_named_folder_falls_back_to_key)
{
    // Not cloud-0-1 - no special label, but still shouldn't print a blank
    // folder name; falls back to the folder's own key.
    const auto folder = buildFolder("cloud-0-2", "", {"Shared Preset"});
    const auto proto = buildFileMessage(folder);

    QcPresetCatalog& catalog = freshCatalog();
    decodeInto(catalog, {proto.data(), proto.size()});

    const auto folders = catalog.folders();
    zassert_equal(folders.size(), 1u, "expected one folder");
    zassert_equal(folders[0].name, "cloud-0-2", "falls back to the folder key");
}

// --- capacity limits ---------------------------------------------------------

ZTEST(preset_catalog, test_preset_capacity_limit_per_folder_is_never_overflowed)
{
    std::vector<std::string_view> slots;
    std::vector<std::string> storage;
    const size_t total = QcPresetCatalog::kMaxPresetsPerFolder + 5;
    storage.reserve(total);
    for (size_t i = 0; i < total; ++i) {
        storage.push_back("Preset " + std::to_string(i));
        slots.push_back(storage.back());
    }

    const auto folder = buildFolder("1_q", "My Presets", slots);
    const auto proto = buildFileMessage(folder);

    QcPresetCatalog& catalog = freshCatalog();
    decodeInto(catalog, {proto.data(), proto.size()});

    const auto folders = catalog.folders();
    zassert_equal(folders.size(), 1u, "expected one folder");
    zassert_equal(folders[0].presets.size(), QcPresetCatalog::kMaxPresetsPerFolder,
                  "never grows past the per-folder capacity");
    zassert_equal(catalog.presetOverflowCount(), 5u, "the 5 extra presets are counted as overflow");
    zassert_true(catalog.overflowed(), "overflowed() reflects the preset overflow");
    zassert_equal(catalog.folderOverflowCount(), 0u, "no folder overflow in this case");
}

ZTEST(preset_catalog, test_folder_capacity_limit_is_never_overflowed)
{
    QcPresetCatalog& catalog = freshCatalog();
    for (size_t i = 0; i < QcPresetCatalog::kMaxFolders + 2; ++i) {
        const std::string key = "folder-" + std::to_string(i);
        const std::string presetName = "Preset " + std::to_string(i);
        const auto folder = buildFolder(key, key, {presetName});
        const auto proto = buildFileMessage(folder);
        decodeInto(catalog, {proto.data(), proto.size()});
    }

    zassert_equal(catalog.folders().size(), QcPresetCatalog::kMaxFolders,
                  "never grows past the folder capacity");
    zassert_equal(catalog.folderOverflowCount(), 2u, "the 2 extra folders are counted as overflow");
    zassert_true(catalog.overflowed(), "overflowed() reflects the folder overflow");
}

// --- reset between listings -------------------------------------------------

ZTEST(preset_catalog, test_reset_clears_a_previous_listing)
{
    const auto folder = buildFolder("1_q", "My Presets", {"Lead Tone", "Clean Rhythm"});
    const auto proto = buildFileMessage(folder);

    QcPresetCatalog& catalog = freshCatalog();
    decodeInto(catalog, {proto.data(), proto.size()});
    zassert_equal(catalog.presetCount(), 2u, "sanity check before reset");
    const uint32_t generationBefore = catalog.generation();

    catalog.reset();

    zassert_equal(catalog.presetCount(), 0u, "presets cleared");
    zassert_true(catalog.folders().empty(), "folders cleared");
    zassert_equal(catalog.folderOverflowCount(), 0u, "folder overflow cleared");
    zassert_equal(catalog.presetOverflowCount(), 0u, "preset overflow cleared");
    zassert_not_equal(catalog.generation(), generationBefore, "generation bumped by reset");

    // A fresh listing after reset must not carry over the old one.
    const auto newFolder = buildFolder("2_q", "Practice", {"Practice 1", "Dark Jazz"});
    const auto newProto = buildFileMessage(newFolder);
    decodeInto(catalog, {newProto.data(), newProto.size()});

    const auto folders = catalog.folders();
    zassert_equal(folders.size(), 1u, "only the new folder is present");
    zassert_equal(folders[0].name, "Practice", "new folder name");
    zassert_equal(folders[0].presets.size(), 2u, "new folder presets");
    zassert_equal(catalog.presetCount(), 2u, "preset count scoped to the new listing");
}

// --- long names don't overflow their fixed storage --------------------------

ZTEST(preset_catalog, test_long_preset_name_is_truncated_not_overflowed)
{
    const std::string longName(200, 'p');
    const auto folder = buildFolder("1_q", "My Presets", {longName});
    const auto proto = buildFileMessage(folder);

    QcPresetCatalog& catalog = freshCatalog();
    decodeInto(catalog, {proto.data(), proto.size()});

    const auto folders = catalog.folders();
    zassert_equal(folders.size(), 1u, "expected one folder");
    zassert_equal(folders[0].presets.size(), 1u, "expected one preset");
    zassert_true(folders[0].presets[0].name.size() <= QcPresetCatalog::kNameCapacity,
                 "name is bounded by kNameCapacity, not left as a dangling view");
}
