// Issue #75: pure-logic coverage for the WebSocket envelope
// (QcLiveUpdateEnvelope.hpp) and the tally-forwarding fix
// (QcMessageTally.hpp's shouldForwardTallyEntry()/isPresetChangeCommand()).
// Neither needs Zephyr's kernel/networking stack, so this mirrors
// ../preset_transport's own "plain C++23, directly unit-testable" suite
// rather than ../wifi_web_http's real-socket one - the real-socket,
// end-to-end shape of these messages (actually sent and received over a
// WebSocket) is covered there instead.

#include <cstdint>
#include <string_view>

#include <zephyr/ztest.h>

#include "QcCatalogJson.hpp"
#include "QcLiveUpdateEnvelope.hpp"
#include "QcMessageTally.hpp"
#include "QcPresetCatalog.hpp"
#include "QcPresetTransport.hpp"

using namespace qcbridge;

ZTEST_SUITE(live_updates, NULL, NULL, NULL, NULL, NULL);

namespace {

// A tiny real catalog, encoded through the same tested encoder every other
// catalog goes through (tests/preset_transport), not a hand-written
// literal - consistent with that suite's own reasoning.
void buildOnePresetCatalogJson(etl::vector<char, 512>& out) {
    static QcPresetCatalog catalog;
    catalog.reset();
    catalog.addPreset(FilePreset{
        .folderKey = "my", .folderName = "My Presets", .presetName = "Dark Jazz", .index = 0});

    static QcCatalogSnapshot snapshot;
    copyCatalogSnapshot(catalog, /*incomplete=*/false, snapshot);
    encodeCatalogJson(snapshot, out);
}

}  // namespace

ZTEST(live_updates, test_snapshot_envelope_wraps_the_catalog_with_seq) {
    etl::vector<char, 512> catalogJson;
    buildOnePresetCatalogJson(catalogJson);

    etl::vector<char, 1024> envelope;
    encodeSnapshotEnvelope(7, catalogJson, envelope);
    const std::string_view text(envelope.data(), envelope.size());

    zassert_true(text.starts_with(R"({"v":1,"t":"snapshot","seq":7,"catalog":)"), "wrong prefix: %.80s",
                 text.data());
    zassert_true(text.ends_with("}"), "doesn't close cleanly");
    zassert_true(text.find(R"("name":"Dark Jazz")") != std::string_view::npos, "catalog not embedded");
    // The embedded catalog closes with its own "}" right before this
    // envelope's own closing "}" - so the whole thing must end in "}}" if
    // the catalog itself was emitted as a complete, valid object (not
    // truncated or missing its own closing brace).
    zassert_true(text.ends_with("}}"), "catalog's own closing brace missing");
}

ZTEST(live_updates, test_patch_envelope_carries_last_change) {
    etl::vector<char, 128> envelope;
    encodePatchEnvelope(3, QcPresetChangeEvent{.command = 15, .count = 2}, envelope);
    const std::string_view text(envelope.data(), envelope.size());

    zassert_equal(text, std::string_view(R"({"v":1,"t":"patch","seq":3,"lastChange":{"command":15,)"
                                         R"("count":2}})"));
}

ZTEST(live_updates, test_tick_envelope_has_no_payload_beyond_the_envelope) {
    etl::vector<char, 64> envelope;
    encodeTickEnvelope(42, envelope);
    const std::string_view text(envelope.data(), envelope.size());

    zassert_equal(text, std::string_view(R"({"v":1,"t":"tick","seq":42})"));
}

ZTEST(live_updates, test_envelope_seq_is_whatever_the_caller_passes_in) {
    // Issue #75: seq continuity is the caller's (QcWifiWebTransport's)
    // responsibility - these encoders just format whatever value they're
    // given. Two calls with different seqs must differ only in that field.
    etl::vector<char, 64> first;
    etl::vector<char, 64> second;
    encodeTickEnvelope(0, first);
    encodeTickEnvelope(1, second);

    zassert_equal(std::string_view(first.data(), first.size()), R"({"v":1,"t":"tick","seq":0})");
    zassert_equal(std::string_view(second.data(), second.size()), R"({"v":1,"t":"tick","seq":1})");
}

ZTEST(live_updates, test_resync_message_is_recognized) {
    zassert_true(isResyncMessage(R"({"v":1,"t":"resync"})"));
}

ZTEST(live_updates, test_other_messages_are_not_mistaken_for_resync) {
    zassert_false(isResyncMessage(R"({"v":1,"t":"switchPreset","folder":0,"index":1})"));
    zassert_false(isResyncMessage(""));
    zassert_false(isResyncMessage("not json at all"));
}

ZTEST(live_updates, test_preset_change_commands_are_classified_per_QcMessageTally_own_list) {
    // RecallPreset, Scene, Grid, RecentsFavorites, PresetDirty,
    // SetlistPosition - QcMessageTally.hpp's own documented list.
    zassert_true(isPresetChangeCommand(kCommandRecallPreset));
    zassert_true(isPresetChangeCommand(kCommandGrid));
    zassert_true(isPresetChangeCommand(kCommandRecentsFavorites));
    zassert_true(isPresetChangeCommand(13));  // Scene
    zassert_true(isPresetChangeCommand(34));  // PresetDirty
    zassert_true(isPresetChangeCommand(2));   // SetlistPosition

    // GlobalTempo (33) is the continuously-streaming command the 5068403
    // smoke test's "command=33" lines are about - must stay throttled.
    zassert_false(isPresetChangeCommand(33));
    zassert_false(isPresetChangeCommand(17));  // MasterVolume
}

ZTEST(live_updates, test_a_brand_new_command_always_forwards) {
    zassert_true(shouldForwardTallyEntry(/*isNew=*/true, /*bumped=*/false, /*intervalDue=*/false, 33));
}

ZTEST(live_updates, test_a_repeat_preset_change_forwards_even_when_the_window_is_not_due) {
    // The issue #75 fix: a real preset switch (RecallPreset) must not wait
    // for the shared intervalDue window GlobalTempo's own ~4/s ticking
    // keeps resetting - the 5068403 smoke test's "command=33 count=2" ->
    // "count=170" a minute apart.
    zassert_true(shouldForwardTallyEntry(/*isNew=*/false, /*bumped=*/true, /*intervalDue=*/false,
                                         kCommandRecallPreset));
}

ZTEST(live_updates, test_a_repeat_continuous_command_stays_throttled) {
    // GlobalTempo bumping without the window being due must NOT forward -
    // otherwise this fix would turn its own ~4/s ticking back into a
    // reprint on every heartbeat, the exact thing intervalDue exists to
    // prevent.
    zassert_false(shouldForwardTallyEntry(/*isNew=*/false, /*bumped=*/true, /*intervalDue=*/false, 33));
    zassert_true(shouldForwardTallyEntry(/*isNew=*/false, /*bumped=*/true, /*intervalDue=*/true, 33));
}

ZTEST(live_updates, test_an_unbumped_entry_never_forwards) {
    zassert_false(
        shouldForwardTallyEntry(/*isNew=*/false, /*bumped=*/false, /*intervalDue=*/true, kCommandRecallPreset));
    zassert_false(shouldForwardTallyEntry(/*isNew=*/false, /*bumped=*/false, /*intervalDue=*/true, 33));
}
