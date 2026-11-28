// Issue #99: the web API protocol session (QcWebApiSession.hpp), the
// socket-free half of the docs/api contract - events in, frames out. Every
// Board -> page frame asserted here is compared byte for byte with the
// contract's own examples (docs/api/examples, embedded at build time by
// cmake/ApiExamples.cmake) wherever one exists, and with a literal written
// from docs/api/README.md otherwise.

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string_view>
#include <variant>

#include <zephyr/ztest.h>

#include "QcCatalogSnapshot.hpp"
#include "QcWebApiSession.hpp"
#include "qc_api_examples_generated.hpp"

using namespace qcbridge;
namespace examples = qcbridge::apiexamples;

ZTEST_SUITE(web_api_session, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr std::string_view kBuild = "85ebe14";
constexpr int64_t kTickIntervalMs = 5000;

// ~33 KB - static, never a test-function local (tests/preset_transport's
// own rule, and the stack fault that taught it).
QcWebApiSession& freshSession() {
    static std::optional<QcWebApiSession> session;
    return session.emplace(kBuild, kTickIntervalMs);
}

// The Catalog docs/api/examples/server/snapshot.json shows: two Folders
// of two Presets, indexed across the whole Catalog.
const QcCatalogSnapshot& exampleCatalog() {
    static QcCatalogSnapshot catalog;
    catalog.folders.clear();
    catalog.generation = 7;
    catalog.presetCount = 4;
    catalog.incomplete = false;
    const struct {
        const char* folder;
        const char* presets[2];
    } kFolders[] = {{"My Presets", {"Clean Chime", "Edge Crunch"}}, {"Live Set", {"Opener", "Verse 1"}}};
    uint32_t index = 0;
    for (const auto& folder : kFolders) {
        catalog.folders.emplace_back();
        catalog.folders.back().name = folder.folder;
        for (const char* name : folder.presets) {
            catalog.folders.back().presets.emplace_back();
            catalog.folders.back().presets.back().index = index++;
            catalog.folders.back().presets.back().name = name;
        }
    }
    return catalog;
}

QcCurrentPreset preset(uint32_t index, const char* name) {
    QcCatalogSnapshot::PresetEntry entry;
    entry.index = index;
    entry.name = name;
    return entry;
}

// Fails the test with the frame that did arrive, so a mismatch shows both.
void assertFrame(const std::optional<std::string_view>& frame, std::string_view expected) {
    zassert_true(frame.has_value(), "no frame, expected %.*s", static_cast<int>(expected.size()),
                 expected.data());
    zassert_equal(*frame, expected, "got      %.*s\nexpected %.*s", static_cast<int>(frame->size()),
                  frame->data(), static_cast<int>(expected.size()), expected.data());
}

void assertNoFrame(const std::optional<std::string_view>& frame) {
    zassert_false(frame.has_value(), "unexpected frame %.*s", static_cast<int>(frame->size()),
                  frame->data());
}

}  // namespace

// The session starts from a hand-written copy of this, so the two must not
// drift apart.
ZTEST(web_api_session, test_the_empty_catalog_is_what_the_encoder_makes_of_an_empty_incomplete_one) {
    static QcCatalogSnapshot empty;
    empty.incomplete = true;
    static etl::vector<char, QcWebApiSession::kCatalogJsonCapacity> json;
    json.clear();

    encodeCatalogJson(empty, json);

    zassert_equal(std::string_view(json.data(), json.size()), QcWebApiSession::kEmptyCatalogJson,
                  "encoder: %.*s", static_cast<int>(json.size()), json.data());
}

ZTEST(web_api_session, test_a_page_connecting_before_the_listing_gets_the_empty_snapshot) {
    QcWebApiSession& session = freshSession();

    assertFrame(session.connectionOpened(0), examples::kServerSnapshotBeforeListing);
}

// docs/api/README.md's "Sequence numbers": every message on a Connection is
// one more than the last, ticks and errors included, and a resync starts a
// new count at a fresh snapshot. The examples are numbered as one such
// Connection, so this replays it and expects each of them in turn.
ZTEST(web_api_session, test_seq_runs_through_snapshot_patch_tick_error_and_restarts_on_resync) {
    QcWebApiSession& session = freshSession();
    assertNoFrame(session.catalogPublished(exampleCatalog(), 0));
    assertNoFrame(session.currentPresetChanged(preset(2, "Opener"), 0));
    assertNoFrame(session.linkChanged(QcLinkState{.qcConnected = true}, 0));

    assertFrame(session.connectionOpened(1000), examples::kServerSnapshot);
    assertFrame(session.currentPresetChanged(preset(3, "Verse 1"), 2000), examples::kServerPatchCurrent);
    assertFrame(session.timePassed(7000), examples::kServerTick);
    assertFrame(session.linkChanged(QcLinkState{.qcConnected = false}, 8000),
                R"({"t":"patch","seq":3,"link":{"qcConnected":false}})");
    assertFrame(session.linkChanged(QcLinkState{.qcConnected = true}, 9000), examples::kServerPatchLink);
    assertFrame(session.frameReceived("not json", 8, 10000), examples::kServerErrorBadRequest);

    assertFrame(session.frameReceived(examples::kClientResync, examples::kClientResync.size(), 11000),
                R"({"t":"snapshot","seq":0,"build":"85ebe14","catalog":{"generation":7,"presetCount":4,)"
                R"("incomplete":false,"folders":[{"name":"My Presets","presets":[{"index":0,"name":)"
                R"("Clean Chime"},{"index":1,"name":"Edge Crunch"}]},{"name":"Live Set","presets":[)"
                R"({"index":2,"name":"Opener"},{"index":3,"name":"Verse 1"}]}]},"current":{"index":3,)"
                R"("name":"Verse 1","scene":null},"link":{"qcConnected":true}})");
    assertFrame(session.timePassed(16000), R"({"t":"tick","seq":1})");
}

// The example's seq 3 is a point in a Connection the replay above doesn't
// reach, so it's checked at the encoder directly.
ZTEST(web_api_session, test_error_encoder_matches_the_stale_catalog_example) {
    static QcWebApiSession::Frame frame;
    encodeErrorMessage(3, std::string_view("c7"), QcWebApiErrorCode::StaleCatalog, "generation 6 != 7", frame);
    zassert_equal(std::string_view(frame.data(), frame.size()), examples::kServerErrorStaleCatalog);
}

ZTEST(web_api_session, test_a_catalog_change_is_a_new_snapshot_never_a_patch) {
    QcWebApiSession& session = freshSession();
    assertFrame(session.connectionOpened(0), examples::kServerSnapshotBeforeListing);
    assertFrame(session.timePassed(5000), R"({"t":"tick","seq":1})");

    assertFrame(session.catalogPublished(exampleCatalog(), 6000),
                R"({"t":"snapshot","seq":0,"build":"85ebe14","catalog":{"generation":7,"presetCount":4,)"
                R"("incomplete":false,"folders":[{"name":"My Presets","presets":[{"index":0,"name":)"
                R"("Clean Chime"},{"index":1,"name":"Edge Crunch"}]},{"name":"Live Set","presets":[)"
                R"({"index":2,"name":"Opener"},{"index":3,"name":"Verse 1"}]}]},"current":{"index":null,)"
                R"("name":null,"scene":null},"link":{"qcConnected":false}})");
    assertFrame(session.timePassed(11000), R"({"t":"tick","seq":1})");
}

ZTEST(web_api_session, test_a_tick_goes_out_only_after_the_interval_with_nothing_else_sent) {
    QcWebApiSession& session = freshSession();
    assertFrame(session.connectionOpened(0), examples::kServerSnapshotBeforeListing);
    assertNoFrame(session.timePassed(3999));
    assertFrame(session.linkChanged(QcLinkState{.qcConnected = true}, 4000),
                R"({"t":"patch","seq":1,"link":{"qcConnected":true}})");

    assertNoFrame(session.timePassed(8999));
    assertFrame(session.timePassed(9000), R"({"t":"tick","seq":2})");
    assertNoFrame(session.timePassed(13999));
    assertFrame(session.timePassed(14000), R"({"t":"tick","seq":3})");
}

// docs/api/README.md, "Liveness": at least one message every 5 s. The
// transport sleeps no longer than this, so the tick goes out when due
// rather than up to one poll interval late.
ZTEST(web_api_session, test_the_time_until_the_next_tick_counts_from_the_last_message_sent) {
    QcWebApiSession& session = freshSession();
    zassert_false(session.msUntilTickDue(0).has_value(), "no page, no tick to wait for");

    (void)session.connectionOpened(1000);
    zassert_equal(session.msUntilTickDue(1000), 5000);
    zassert_equal(session.msUntilTickDue(4800), 1200);
    (void)session.linkChanged(QcLinkState{.qcConnected = true}, 4800);
    zassert_equal(session.msUntilTickDue(4800), 5000);
    zassert_equal(session.msUntilTickDue(9800), 0);
    zassert_equal(session.msUntilTickDue(12000), 0, "an overdue tick is due now, not negative");

    session.connectionClosed();
    zassert_false(session.msUntilTickDue(12000).has_value());
}

ZTEST(web_api_session, test_an_unchanged_current_preset_or_link_sends_nothing) {
    QcWebApiSession& session = freshSession();
    (void)session.catalogPublished(exampleCatalog(), 0);
    (void)session.currentPresetChanged(preset(2, "Opener"), 0);
    (void)session.linkChanged(QcLinkState{.qcConnected = true}, 0);
    assertFrame(session.connectionOpened(0), examples::kServerSnapshot);

    assertNoFrame(session.currentPresetChanged(preset(2, "Opener"), 100));
    assertNoFrame(session.linkChanged(QcLinkState{.qcConnected = true}, 100));
    assertFrame(session.currentPresetChanged(QcCurrentPreset{}, 200),
                R"({"t":"patch","seq":1,"current":{"index":null,"name":null,"scene":null}})");
}

ZTEST(web_api_session, test_nothing_is_sent_without_a_page_and_the_next_page_sees_the_latest_state) {
    QcWebApiSession& session = freshSession();
    assertFrame(session.connectionOpened(0), examples::kServerSnapshotBeforeListing);
    session.connectionClosed();

    assertNoFrame(session.catalogPublished(exampleCatalog(), 100));
    assertNoFrame(session.currentPresetChanged(preset(2, "Opener"), 100));
    assertNoFrame(session.linkChanged(QcLinkState{.qcConnected = true}, 100));
    assertNoFrame(session.timePassed(60000));
    assertNoFrame(session.frameReceived(examples::kClientResync, examples::kClientResync.size(), 60000));

    assertFrame(session.connectionOpened(60000), examples::kServerSnapshot);
}

// --- Page -> Board frames ----------------------------------------------------

ZTEST(web_api_session, test_the_client_examples_parse) {
    zassert_true(std::holds_alternative<QcWebApiResync>(
        parseWebApiRequest(examples::kClientResync, examples::kClientResync.size())));

    const auto request = parseWebApiRequest(examples::kClientSwitchPreset, examples::kClientSwitchPreset.size());
    const auto* switchPreset = std::get_if<QcWebApiSwitchPreset>(&request);
    zassert_not_null(switchPreset, "switch-preset.json didn't parse as a switchPreset");
    zassert_equal(switchPreset->id, "c7");
    zassert_equal(switchPreset->index, 3u);
    zassert_equal(switchPreset->generation, 7u);
}

// --- Switching a Preset (issue #78, docs/api/README.md) --------------------

namespace {

constexpr int64_t kSwitchTimeoutMs = 4000;

std::string_view switchFrame(std::string_view id, uint32_t index, uint32_t generation) {
    static char text[96];
    const int n = snprintf(text, sizeof(text), R"({"t":"switchPreset","id":"%.*s","index":%u,"generation":%u})",
                           static_cast<int>(id.size()), id.data(), index, generation);
    return {text, static_cast<size_t>(n)};
}

std::optional<std::string_view> receive(QcWebApiSession& session, std::string_view frame, int64_t nowMs) {
    return session.frameReceived(frame, frame.size(), nowMs);
}

// The example Board, Link up, with the example Catalog (generation 7, four
// Presets) and Opener (2) loaded, and a page that just got its snapshot -
// so the next frame is seq 1.
QcWebApiSession& connectedSession() {
    QcWebApiSession& session = freshSession();
    (void)session.linkChanged(QcLinkState{.qcConnected = true}, 0);
    (void)session.catalogPublished(exampleCatalog(), 0);
    (void)session.currentPresetChanged(preset(2, "Opener"), 0);
    assertFrame(session.connectionOpened(0), examples::kServerSnapshot);
    return session;
}

void assertSwitchRequested(QcWebApiSession& session, uint32_t generation, uint32_t index) {
    const auto request = session.takeSwitchRequest();
    zassert_true(request.has_value(), "no switch was asked of the Quad Cortex");
    zassert_equal(request->generation, generation);
    zassert_equal(request->index, index);
    zassert_false(session.takeSwitchRequest().has_value(), "a switch is asked for once");
}

}  // namespace

ZTEST(web_api_session, test_an_accepted_switch_asks_the_quad_cortex_once_and_answers_nothing_yet) {
    QcWebApiSession& session = connectedSession();

    assertNoFrame(receive(session, examples::kClientSwitchPreset, 100));

    assertSwitchRequested(session, 7, 3);
}

ZTEST(web_api_session, test_the_quad_cortex_reporting_the_load_is_the_confirmation) {
    QcWebApiSession& session = connectedSession();
    (void)receive(session, examples::kClientSwitchPreset, 100);

    assertFrame(session.currentPresetChanged(preset(3, "Verse 1"), 500), examples::kServerPatchCurrent);
}

// "It always sends this patch when a switch completes, even if current
// didn't change" - the page clears pending on it.
ZTEST(web_api_session, test_switching_to_the_loaded_preset_still_gets_a_current_patch) {
    QcWebApiSession& session = connectedSession();
    (void)receive(session, switchFrame("c8", 2, 7), 100);

    assertFrame(session.currentPresetChanged(preset(2, "Opener"), 500),
                R"({"t":"patch","seq":1,"current":{"index":2,"name":"Opener","scene":null}})");
    assertNoFrame(session.currentPresetChanged(preset(2, "Opener"), 600));
}

ZTEST(web_api_session, test_a_switch_while_the_link_is_down_is_refused) {
    QcWebApiSession& session = connectedSession();
    (void)session.linkChanged(QcLinkState{.qcConnected = false}, 100);

    const auto frame = receive(session, examples::kClientSwitchPreset, 200);

    zassert_true(frame.has_value() &&
                     frame->starts_with(R"({"t":"error","seq":2,"id":"c7","code":"qcDisconnected",)"),
                 "got %.*s", frame ? static_cast<int>(frame->size()) : 0, frame ? frame->data() : "");
    zassert_false(session.takeSwitchRequest().has_value());
}

ZTEST(web_api_session, test_a_switch_from_an_old_catalog_is_refused_as_stale) {
    QcWebApiSession& session = connectedSession();

    assertFrame(receive(session, switchFrame("c7", 3, 6), 100),
                R"({"t":"error","seq":1,"id":"c7","code":"staleCatalog","message":"generation 6 != 7"})");
    zassert_false(session.takeSwitchRequest().has_value());
}

ZTEST(web_api_session, test_a_switch_past_the_end_of_the_catalog_is_a_bad_index) {
    QcWebApiSession& session = connectedSession();

    assertFrame(receive(session, switchFrame("c7", 4, 7), 100),
                R"({"t":"error","seq":1,"id":"c7","code":"badIndex","message":"no preset 4 in 4"})");
    zassert_false(session.takeSwitchRequest().has_value());
}

ZTEST(web_api_session, test_a_switch_before_the_first_listing_is_a_bad_index) {
    QcWebApiSession& session = freshSession();
    (void)session.linkChanged(QcLinkState{.qcConnected = true}, 0);
    (void)session.connectionOpened(0);

    assertFrame(receive(session, switchFrame("c1", 0, 0), 100),
                R"({"t":"error","seq":1,"id":"c1","code":"badIndex","message":"no preset 0 in 0"})");
}

ZTEST(web_api_session, test_a_second_switch_while_one_is_in_flight_is_busy_and_not_queued) {
    QcWebApiSession& session = connectedSession();
    (void)receive(session, examples::kClientSwitchPreset, 100);
    (void)session.takeSwitchRequest();

    assertFrame(receive(session, switchFrame("c8", 1, 7), 200),
                R"({"t":"error","seq":1,"id":"c8","code":"busy","message":"a switch is in flight"})");
    zassert_false(session.takeSwitchRequest().has_value());
}

ZTEST(web_api_session, test_a_confirmed_switch_frees_the_board_for_the_next_one) {
    QcWebApiSession& session = connectedSession();
    (void)receive(session, examples::kClientSwitchPreset, 100);
    (void)session.takeSwitchRequest();
    (void)session.currentPresetChanged(preset(3, "Verse 1"), 500);

    assertNoFrame(receive(session, switchFrame("c8", 1, 7), 600));
    assertSwitchRequested(session, 7, 1);
}

// "The Board gives up on its side of an in-flight switch within the same
// 4 s, so a lost switch never leaves it stuck at busy."
ZTEST(web_api_session, test_the_board_gives_up_on_an_unconfirmed_switch_after_4_s) {
    QcWebApiSession& session = connectedSession();
    (void)receive(session, examples::kClientSwitchPreset, 100);
    (void)session.takeSwitchRequest();

    assertNoFrame(session.timePassed(100 + kSwitchTimeoutMs - 1));
    zassert_true(receive(session, switchFrame("c8", 1, 7), 100 + kSwitchTimeoutMs - 1)->find("busy") !=
                 std::string_view::npos);
    assertNoFrame(session.timePassed(100 + kSwitchTimeoutMs));

    assertNoFrame(receive(session, switchFrame("c9", 1, 7), 100 + kSwitchTimeoutMs));
    assertSwitchRequested(session, 7, 1);
    // A late report of the given-up switch is an ordinary change again.
    assertFrame(session.currentPresetChanged(preset(1, "Edge Crunch"), 100 + kSwitchTimeoutMs + 10),
                R"({"t":"patch","seq":2,"current":{"index":1,"name":"Edge Crunch","scene":null}})");
}

ZTEST(web_api_session, test_the_link_going_down_drops_the_switch_in_flight) {
    QcWebApiSession& session = connectedSession();
    (void)receive(session, examples::kClientSwitchPreset, 100);
    (void)session.takeSwitchRequest();
    (void)session.linkChanged(QcLinkState{.qcConnected = false}, 200);
    (void)session.linkChanged(QcLinkState{.qcConnected = true}, 300);

    assertNoFrame(receive(session, switchFrame("c8", 1, 7), 400));
    assertSwitchRequested(session, 7, 1);
}

namespace {

struct BadFrame {
    const char* what;
    std::string_view text;
    // How the answer must start: the id is echoed only when it was valid.
    std::string_view expectedStart;
};

constexpr std::string_view kNoId = R"({"t":"error","seq":1,"code":"badRequest",)";
constexpr std::string_view kIdC7 = R"({"t":"error","seq":1,"id":"c7","code":"badRequest",)";

const BadFrame kBadFrames[] = {
    {"invalid/client/switch-preset-long-id.json", examples::kInvalidClientSwitchPresetLongId, kNoId},
    {"invalid/client/switch-preset-no-generation.json", examples::kInvalidClientSwitchPresetNoGeneration, kIdC7},
    {"not JSON", "not json", kNoId},
    {"empty frame", "", kNoId},
    {"truncated object", R"({"t":"resync")", kNoId},
    {"trailing garbage", R"({"t":"resync"}x)", kNoId},
    {"two objects", R"({"t":"resync"}{"t":"resync"})", kNoId},
    {"not an object", R"(["resync"])", kNoId},
    {"bare string", R"("resync")", kNoId},
    {"missing t", R"({"id":"c7"})", kIdC7},
    {"t not a string", R"({"t":5,"id":"c7"})", kIdC7},
    {"unknown t", R"({"t":"loadScene","id":"c7","scene":2})", kIdC7},
    {"unknown t, no id", R"({"t":"loadScene"})", kNoId},
    // Not a raw string: GCC turns s back into 's' even inside one.
    {"t with an escape", "{\"t\":\"re\\u0073ync\"}", kNoId},
    {"missing id", R"({"t":"switchPreset","index":3,"generation":7})", kNoId},
    {"empty id", R"({"t":"switchPreset","id":"","index":3,"generation":7})", kNoId},
    {"id with a space", R"({"t":"switchPreset","id":"c 7","index":3,"generation":7})", kNoId},
    {"id not a string", R"({"t":"switchPreset","id":7,"index":3,"generation":7})", kNoId},
    {"missing index", R"({"t":"switchPreset","id":"c7","generation":7})", kIdC7},
    {"negative index", R"({"t":"switchPreset","id":"c7","index":-1,"generation":7})", kIdC7},
    {"fractional index", R"({"t":"switchPreset","id":"c7","index":3.5,"generation":7})", kIdC7},
    {"exponent index", R"({"t":"switchPreset","id":"c7","index":3e0,"generation":7})", kIdC7},
    {"index over 65535", R"({"t":"switchPreset","id":"c7","index":65536,"generation":7})", kIdC7},
    {"index as a string", R"({"t":"switchPreset","id":"c7","index":"3","generation":7})", kIdC7},
    {"index null", R"({"t":"switchPreset","id":"c7","index":null,"generation":7})", kIdC7},
    {"generation over 32 bits", R"({"t":"switchPreset","id":"c7","index":3,"generation":4294967296})", kIdC7},
    {"leading zero", R"({"t":"switchPreset","id":"c7","index":03,"generation":7})", kNoId},
    {"bad escape", R"({"t":"resync","x":"\q"})", kNoId},
    {"control character in a string", "{\"t\":\"resync\",\"x\":\"a\tb\"}", kNoId},
    {"nesting too deep", R"({"t":"resync","x":[[[[[[[[[[1]]]]]]]]]]})", kNoId},
};

}  // namespace

ZTEST(web_api_session, test_every_malformed_frame_gets_one_bad_request) {
    for (const BadFrame& bad : kBadFrames) {
        QcWebApiSession& session = freshSession();
        (void)session.connectionOpened(0);

        const auto frame = session.frameReceived(bad.text, bad.text.size(), 0);

        zassert_true(frame.has_value(), "%s: no answer", bad.what);
        zassert_true(frame->starts_with(bad.expectedStart), "%s: got %.*s", bad.what,
                     static_cast<int>(frame->size()), frame->data());
    }
}

ZTEST(web_api_session, test_a_frame_over_256_bytes_is_refused_even_if_it_would_parse) {
    // The transport reads at most 256 bytes and passes the full length
    // alongside them, so a valid-looking start doesn't make it acceptable.
    QcWebApiSession& session = freshSession();
    (void)session.connectionOpened(0);

    assertFrame(session.frameReceived(examples::kClientResync, 257, 0),
                R"({"t":"error","seq":1,"code":"badRequest","message":"frame too large"})");
}

ZTEST(web_api_session, test_fields_and_whitespace_the_board_doesnt_know_are_ignored) {
    const std::string_view text =
        R"( { "x" : {"nested":[1,true,null,"s\"qé"]} , "t" : "resync", "seq": -1.5e+3 } )";
    zassert_true(std::holds_alternative<QcWebApiResync>(parseWebApiRequest(text, text.size())));
}
