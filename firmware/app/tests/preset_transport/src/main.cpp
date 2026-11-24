#include <cstdint>
#include <cstdio>
#include <string_view>

#include <zephyr/ztest.h>

#include "QcCatalogJson.hpp"
#include "QcCatalogSnapshot.hpp"
#include "QcFileMessage.hpp"
#include "QcPresetCatalog.hpp"
#include "QcPresetTransport.hpp"

using namespace qcbridge;

ZTEST_SUITE(preset_transport, NULL, NULL, NULL, NULL, NULL);

namespace {

void addPreset(QcPresetCatalog& catalog, std::string_view folderKey, std::string_view folderName,
                std::string_view presetName, uint32_t index) {
    catalog.addPreset(FilePreset{
        .folderKey = folderKey, .folderName = folderName, .presetName = presetName, .index = index});
}

// QcPresetCatalog/QcCatalogSnapshot are each several KB (8 folders x 64
// presets x a ~32-byte name) - too big for a Ztest thread's stack as a
// per-test local, same "big objects go in static storage, never a stack
// temporary" lesson this firmware's own hardware crashes already taught
// (confirmed here too: the first version of this suite faulted - zeroed
// registers, PC 0 - with both as stack locals). One instance of each,
// reset before every test, mirroring tests/preset_catalog's freshCatalog().
QcPresetCatalog& freshCatalog() {
    static QcPresetCatalog catalog;
    catalog.reset();
    return catalog;
}

QcCatalogSnapshot& freshSnapshot() {
    static QcCatalogSnapshot snapshot;
    return snapshot;
}

// issue #71's seam proof: a transport that only needs plain fields - no
// Zephyr/Wi-Fi/HTTP dependency at all - standing in for a real
// QcWifiWebTransport (or a future BLE one) to show QcSession's own
// addTransport()/publishCatalog()/publishCurrentPreset()/publishLink() call
// pattern works against nothing but this interface.
class FakeTransport : public QcPresetTransport {
public:
    void publishCatalog(const QcCatalogSnapshot& snapshot) override {
        ++catalogPublishCount;
        lastPresetCount = snapshot.presetCount;
        lastGeneration = snapshot.generation;
        lastIncomplete = snapshot.incomplete;
    }

    void publishCurrentPreset(const QcCurrentPreset& current) override {
        ++currentPublishCount;
        lastCurrent = current;
    }

    void publishLink(const QcLinkState& link) override {
        ++linkPublishCount;
        lastLink = link;
    }

    size_t catalogPublishCount = 0;
    size_t currentPublishCount = 0;
    size_t linkPublishCount = 0;
    size_t lastPresetCount = 0;
    uint32_t lastGeneration = 0;
    bool lastIncomplete = false;
    QcCurrentPreset lastCurrent;
    QcLinkState lastLink;
};

}  // namespace

ZTEST(preset_transport, test_fake_transport_receives_catalog_current_preset_and_link) {
    QcPresetCatalog& catalog = freshCatalog();
    addPreset(catalog, "my-presets", "My Presets", "Practice 1", 0);
    addPreset(catalog, "my-presets", "My Presets", "Dark Jazz", 1);

    QcCatalogSnapshot& snapshot = freshSnapshot();
    copyCatalogSnapshot(catalog, /*incomplete=*/false, snapshot);

    // Mirrors QcSession::addTransport()'s kMaxTransports=2 storage and the
    // for-loop in logFileStreamSummary()/runHeartbeat() that calls every
    // registered transport - proving the seam doesn't need QcSession
    // itself, a real Wi-Fi socket, or Zephyr kernel primitives to exercise.
    FakeTransport wifi;
    FakeTransport ble;
    etl::vector<QcPresetTransport*, 2> transports;
    transports.push_back(&wifi);
    transports.push_back(&ble);

    for (auto* transport : transports) {
        transport->publishCatalog(snapshot);
    }
    for (auto* transport : transports) {
        transport->publishCurrentPreset(findCurrentPreset(catalog, "my-presets", 1));
        transport->publishLink(QcLinkState{.qcConnected = true});
    }

    zassert_equal(wifi.catalogPublishCount, 1);
    zassert_equal(ble.catalogPublishCount, 1);
    zassert_equal(wifi.lastPresetCount, 2);
    zassert_equal(wifi.lastGeneration, snapshot.generation);
    zassert_false(wifi.lastIncomplete);
    zassert_equal(wifi.currentPublishCount, 1);
    zassert_true(wifi.lastCurrent.has_value());
    zassert_equal(wifi.lastCurrent->name, "Dark Jazz");
    zassert_equal(wifi.linkPublishCount, 1);
    zassert_true(wifi.lastLink.qcConnected);
    zassert_equal(ble.currentPublishCount, 1);
    zassert_equal(ble.linkPublishCount, 1);
}

ZTEST(preset_transport, test_copy_catalog_snapshot_basic) {
    QcPresetCatalog& catalog = freshCatalog();
    addPreset(catalog, "my-presets", "My Presets", "Practice 1", 0);
    addPreset(catalog, "my-presets", "My Presets", "Dark Jazz", 5);
    addPreset(catalog, "cloud-0-1", "", "Salaatti-Classic", 2);

    QcCatalogSnapshot& snapshot = freshSnapshot();
    copyCatalogSnapshot(catalog, /*incomplete=*/true, snapshot);

    zassert_equal(snapshot.folders.size(), 2);
    zassert_equal(snapshot.presetCount, 3);
    zassert_true(snapshot.incomplete);
    zassert_equal(snapshot.generation, catalog.generation());

    zassert_equal(snapshot.folders[0].name, "My Presets");
    zassert_equal(snapshot.folders[0].presets.size(), 2);
    zassert_equal(snapshot.folders[0].presets[0].name, "Practice 1");
    zassert_equal(snapshot.folders[0].presets[1].name, "Dark Jazz");

    // cloud-0-1's real empty folder name is special-cased to "Downloads" -
    // QcPresetCatalog.hpp's own labelFor(), exercised here through the
    // snapshot copy rather than re-tested directly.
    zassert_equal(snapshot.folders[1].name, "Downloads");
    zassert_equal(snapshot.folders[1].presets[0].name, "Salaatti-Classic");
}

// Issue #99 / docs/api: a Preset's index counts across the whole Catalog,
// in listing order - not its slot within its Folder (Dark Jazz sits in
// slot 5 of My Presets, and Salaatti-Classic in slot 2 of Downloads).
ZTEST(preset_transport, test_snapshot_indexes_presets_across_the_whole_catalog) {
    QcPresetCatalog& catalog = freshCatalog();
    addPreset(catalog, "my-presets", "My Presets", "Practice 1", 0);
    addPreset(catalog, "my-presets", "My Presets", "Dark Jazz", 5);
    addPreset(catalog, "cloud-0-1", "", "Salaatti-Classic", 2);

    QcCatalogSnapshot& snapshot = freshSnapshot();
    copyCatalogSnapshot(catalog, /*incomplete=*/false, snapshot);

    zassert_equal(snapshot.folders[0].presets[0].index, 0);
    zassert_equal(snapshot.folders[0].presets[1].index, 1);
    zassert_equal(snapshot.folders[1].presets[0].index, 2);
}

// The Current preset comes from the Quad Cortex as a Folder key and a slot
// in that Folder (SetlistPosition); the page addresses it by the same
// Catalog-wide index the snapshot gives it.
ZTEST(preset_transport, test_current_preset_is_found_by_folder_key_and_slot) {
    QcPresetCatalog& catalog = freshCatalog();
    addPreset(catalog, "my-presets", "My Presets", "Practice 1", 0);
    addPreset(catalog, "my-presets", "My Presets", "Dark Jazz", 5);
    addPreset(catalog, "cloud-0-1", "", "Salaatti-Classic", 2);

    const QcCurrentPreset darkJazz = findCurrentPreset(catalog, "my-presets", 5);
    zassert_true(darkJazz.has_value());
    zassert_equal(darkJazz->index, 1u);
    zassert_equal(darkJazz->name, "Dark Jazz");

    const QcCurrentPreset salaatti = findCurrentPreset(catalog, "cloud-0-1", 2);
    zassert_true(salaatti.has_value());
    zassert_equal(salaatti->index, 2u);
    zassert_equal(salaatti->name, "Salaatti-Classic");
}

ZTEST(preset_transport, test_current_preset_is_unknown_outside_the_catalog) {
    QcPresetCatalog& catalog = freshCatalog();
    addPreset(catalog, "my-presets", "My Presets", "Dark Jazz", 5);

    zassert_false(findCurrentPreset(catalog, "my-presets", 4).has_value(), "an empty slot");
    zassert_false(findCurrentPreset(catalog, "practice", 5).has_value(), "a folder with no named preset");
    zassert_false(findCurrentPreset(freshCatalog(), "my-presets", 5).has_value(), "before any listing");
}

ZTEST(preset_transport, test_encode_catalog_json_basic) {
    QcPresetCatalog& catalog = freshCatalog();
    addPreset(catalog, "my-presets", "My Presets", "Practice 1", 0);
    addPreset(catalog, "my-presets", "My Presets", "Dark Jazz", 5);

    QcCatalogSnapshot& snapshot = freshSnapshot();
    copyCatalogSnapshot(catalog, /*incomplete=*/false, snapshot);

    static etl::vector<char, 1024> json;
    encodeCatalogJson(snapshot, json);
    const std::string_view text(json.data(), json.size());

    // docs/api's key order: incomplete before folders.
    zassert_true(text.starts_with("{\"generation\":"));
    zassert_true(text.ends_with(R"(,"presetCount":2,"incomplete":false,"folders":[{"name":"My Presets",)"
                                R"("presets":[{"index":0,"name":"Practice 1"},{"index":1,"name":"Dark Jazz"}]}]})"),
                 "got %.*s", static_cast<int>(text.size()), text.data());
}

ZTEST(preset_transport, test_encode_catalog_json_escapes_special_characters) {
    QcPresetCatalog& catalog = freshCatalog();
    // Preset names come from the device's own protocol (QcFileMessage.hpp)
    // and aren't guaranteed free of JSON-unsafe characters -
    // QcCatalogJson.hpp's appendJsonEscaped() is what's under test here.
    addPreset(catalog, "my-presets", "My Presets", "Say \"Hi\"\\ok", 0);

    QcCatalogSnapshot& snapshot = freshSnapshot();
    copyCatalogSnapshot(catalog, /*incomplete=*/false, snapshot);

    static etl::vector<char, 512> json;
    encodeCatalogJson(snapshot, json);
    const std::string_view text(json.data(), json.size());

    zassert_true(text.find(R"("name":"Say \"Hi\"\\ok")") != std::string_view::npos);
}

ZTEST(preset_transport, test_encode_catalog_json_truncates_when_undersized) {
    QcPresetCatalog& catalog = freshCatalog();
    for (int i = 0; i < 20; ++i) {
        char name[16];
        snprintf(name, sizeof(name), "Preset %d", i);
        addPreset(catalog, "my-presets", "My Presets", name, static_cast<uint32_t>(i));
    }

    QcCatalogSnapshot& snapshot = freshSnapshot();
    copyCatalogSnapshot(catalog, /*incomplete=*/false, snapshot);

    // Deliberately too small to hold all 20 presets - encodeCatalogJson()
    // must still close every bracket cleanly and flip incomplete to true,
    // never emit a buffer silently cut off mid-string.
    static etl::vector<char, 128> json;
    encodeCatalogJson(snapshot, json);
    const std::string_view text(json.data(), json.size());

    zassert_true(text.find(R"("incomplete":true,"folders":[)") != std::string_view::npos, "got %.*s",
                 static_cast<int>(text.size()), text.data());
    zassert_true(text.starts_with("{\"generation\":"));

    size_t openBraces = 0;
    size_t closeBraces = 0;
    size_t openBrackets = 0;
    size_t closeBrackets = 0;
    for (const char c : text) {
        if (c == '{') ++openBraces;
        if (c == '}') ++closeBraces;
        if (c == '[') ++openBrackets;
        if (c == ']') ++closeBrackets;
    }
    zassert_equal(openBraces, closeBraces);
    zassert_equal(openBrackets, closeBrackets);
}

ZTEST(preset_transport, test_encode_catalog_json_reports_incomplete_from_snapshot) {
    QcPresetCatalog& catalog = freshCatalog();
    addPreset(catalog, "my-presets", "My Presets", "Practice 1", 0);

    QcCatalogSnapshot& snapshot = freshSnapshot();
    // incomplete=true here models QcFileStream::isListingIncomplete()
    // already having found a dropped/overflowed/timed-out listing -
    // encodeCatalogJson() must surface that even when nothing truncates
    // during encoding itself (plenty of buffer room below).
    copyCatalogSnapshot(catalog, /*incomplete=*/true, snapshot);

    static etl::vector<char, 1024> json;
    encodeCatalogJson(snapshot, json);
    const std::string_view text(json.data(), json.size());

    zassert_true(text.find(R"("incomplete":true,"folders":[)") != std::string_view::npos);
}
