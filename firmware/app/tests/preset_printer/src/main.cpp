#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

// QcPresetPrinter.hpp's LOG_INF/LOG_WRN calls (inside printCatalog(), never
// exercised by this suite - it only tests the pure copyPresetEntries()
// helper) still need a module registered in this translation unit, same as
// Main.cpp's LOG_MODULE_REGISTER(main, ...) does for the firmware image.
LOG_MODULE_REGISTER(preset_printer_test, LOG_LEVEL_INF);

#include "QcPresetCatalog.hpp"
#include "QcPresetPrinter.hpp"

using namespace qcbridge;

ZTEST_SUITE(preset_printer, NULL, NULL, NULL, NULL, NULL);

namespace {

QcPresetCatalog::PresetEntry makeEntry(uint32_t index, std::string_view name) {
    QcPresetCatalog::PresetEntry entry;
    entry.index = index;
    entry.name.assign(name.data(), name.size());
    return entry;
}

using Snapshot = std::array<QcPresetCatalog::PresetEntry, QcPresetCatalog::kMaxPresetsPerFolder>;

}  // namespace

// issue #10 Copilot review (QcPresetPrinter.hpp:116): printCatalog() used
// to iterate a folder's live `presets` vector while LOG_INF ran, racing a
// reconnect's reset()/addPreset() on the same storage. The fix copies each
// folder's entries out under the writers' mutex before printing -
// detail::copyPresetEntries() is the pure copy step that does, with no
// locking of its own (the caller holds the lock around calling it).
// These tests cover its only real failure mode: writing past the
// fixed-size destination.

ZTEST(preset_printer, test_copy_preset_entries_copies_fewer_than_capacity)
{
    std::vector<QcPresetCatalog::PresetEntry> source;
    source.push_back(makeEntry(1, "Lead Tone"));
    source.push_back(makeEntry(3, "Clean Rhythm"));

    Snapshot dest{};
    const size_t copied = detail::copyPresetEntries({source.data(), source.size()}, dest);

    zassert_equal(copied, 2u, "expected both entries copied");
    zassert_equal(dest[0].index, 1u, "first entry index");
    zassert_equal(dest[0].name, "Lead Tone", "first entry name");
    zassert_equal(dest[1].index, 3u, "second entry index");
    zassert_equal(dest[1].name, "Clean Rhythm", "second entry name");
}

ZTEST(preset_printer, test_copy_preset_entries_copies_exactly_at_capacity)
{
    std::vector<QcPresetCatalog::PresetEntry> source;
    for (size_t i = 0; i < QcPresetCatalog::kMaxPresetsPerFolder; ++i) {
        source.push_back(makeEntry(static_cast<uint32_t>(i), "P"));
    }

    Snapshot dest{};
    const size_t copied = detail::copyPresetEntries({source.data(), source.size()}, dest);

    zassert_equal(copied, QcPresetCatalog::kMaxPresetsPerFolder, "expected every entry copied");
    zassert_equal(dest[QcPresetCatalog::kMaxPresetsPerFolder - 1].index,
                  static_cast<uint32_t>(QcPresetCatalog::kMaxPresetsPerFolder - 1), "last entry index");
}

ZTEST(preset_printer, test_copy_preset_entries_clamps_a_source_larger_than_capacity)
{
    // QcPresetCatalog's own invariants mean a folder's presets vector
    // never actually grows past kMaxPresetsPerFolder - this is a
    // defensive regression guard that the clamp (std::min) itself never
    // regresses into an out-of-bounds write on `dest`, not a scenario
    // this catalog can produce today.
    std::vector<QcPresetCatalog::PresetEntry> source;
    for (size_t i = 0; i < QcPresetCatalog::kMaxPresetsPerFolder + 5; ++i) {
        source.push_back(makeEntry(static_cast<uint32_t>(i), "P"));
    }

    Snapshot dest{};
    const size_t copied = detail::copyPresetEntries({source.data(), source.size()}, dest);

    zassert_equal(copied, QcPresetCatalog::kMaxPresetsPerFolder,
                  "copy must clamp to dest's capacity, never overrun it");
}

ZTEST(preset_printer, test_copy_preset_entries_handles_an_empty_source)
{
    const std::vector<QcPresetCatalog::PresetEntry> source;
    Snapshot dest{};
    const size_t copied = detail::copyPresetEntries({source.data(), source.size()}, dest);

    zassert_equal(copied, 0u, "an empty folder copies nothing");
}
