#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>

#include "QcHandshakeMessages.hpp"
#include "QcModelRepoMessage.hpp"
#include "QcProtobuf.hpp"
#include "QcSetlistPositionMessage.hpp"

using namespace qcbridge;

ZTEST_SUITE(catalog_messages, NULL, NULL, NULL, NULL, NULL);

namespace {
std::string_view asStringView(const std::vector<uint8_t>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
}  // namespace

// --- QcModelRepoMessage.hpp ---------------------------------------------

ZTEST(catalog_messages, test_decode_model_repo_picks_out_action_request_id_and_payload)
{
    etl::vector<uint8_t, 64> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Read));
    writeUInt64(proto, 2, 7);
    const std::vector<uint8_t> payload = {0x1f, 0x8b, 0x08, 0x00, 0xde, 0xad, 0xbe, 0xef};
    writeString(proto, 3, asStringView(payload));

    const auto reply = decodeModelRepo({proto.data(), proto.size()});
    zassert_true(reply.has_value(), "expected a reply");
    zassert_true(reply->action == MessageAction::Read, "action");
    zassert_equal(reply->requestId, 7u, "requestId");
    zassert_equal(reply->payload.size(), payload.size(), "payload size");
    zassert_mem_equal(reply->payload.data(), payload.data(), payload.size(), "payload bytes");
}

ZTEST(catalog_messages, test_decode_model_repo_missing_payload_yields_empty_span)
{
    etl::vector<uint8_t, 16> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Update));
    writeUInt64(proto, 2, 3);

    const auto reply = decodeModelRepo({proto.data(), proto.size()});
    zassert_true(reply.has_value(), "expected a reply");
    zassert_equal(reply->requestId, 3u, "requestId");
    zassert_true(reply->payload.empty(), "expected empty payload span");
}

ZTEST(catalog_messages, test_decode_model_repo_empty_input_fails)
{
    zassert_false(decodeModelRepo({}).has_value(), "expected nullopt on empty input");
}

// --- QcSetlistPositionMessage.hpp ---------------------------------------

ZTEST(catalog_messages, test_decode_setlist_position_my_presets_addressing)
{
    etl::vector<uint8_t, 128> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Update));
    writeUInt64(proto, 2, 99);
    writeString(proto, 3, "/media/p4/Presets/My Presets");
    writeUInt64(proto, 4, 12);
    writeBool(proto, 5, true);   // is_factory
    writeBool(proto, 6, false);  // is_downloads
    writeBool(proto, 8, false);  // is_plugin

    const auto reply = decodeSetlistPosition({proto.data(), proto.size()});
    zassert_true(reply.has_value(), "expected a reply");
    zassert_true(reply->action == MessageAction::Update, "action");
    zassert_equal(reply->requestId, 99u, "requestId");
    zassert_equal(reply->folderKey.size(), 28u, "folderKey size");
    zassert_mem_equal(reply->folderKey.data(), "/media/p4/Presets/My Presets", 28, "folderKey content");
    zassert_equal(reply->position, 12u, "position");
    zassert_true(reply->isFactory, "isFactory");
    zassert_false(reply->isDownloads, "isDownloads");
    zassert_false(reply->isPlugin, "isPlugin");
    zassert_true(reply->keyInDownloads.empty(), "expected empty keyInDownloads");
    zassert_true(reply->keyInPluginFolder.empty(), "expected empty keyInPluginFolder");
}

ZTEST(catalog_messages, test_decode_setlist_position_downloads_addressing)
{
    etl::vector<uint8_t, 128> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Update));
    writeUInt64(proto, 2, 5);
    writeBool(proto, 6, true);  // is_downloads
    writeString(proto, 7, "cloud-0-1");

    const auto reply = decodeSetlistPosition({proto.data(), proto.size()});
    zassert_true(reply.has_value(), "expected a reply");
    zassert_true(reply->isDownloads, "isDownloads");
    zassert_equal(reply->keyInDownloads.size(), 9u, "keyInDownloads size");
    zassert_mem_equal(reply->keyInDownloads.data(), "cloud-0-1", 9, "keyInDownloads content");
    zassert_true(reply->folderKey.empty(), "expected empty folderKey");
    zassert_equal(reply->position, 0u, "expected default position");
}

ZTEST(catalog_messages, test_decode_setlist_position_missing_fields_yield_defaults)
{
    etl::vector<uint8_t, 16> proto;
    writeUInt64(proto, 2, 1);  // only request_id present

    const auto reply = decodeSetlistPosition({proto.data(), proto.size()});
    zassert_true(reply.has_value(), "expected a reply");
    zassert_equal(reply->position, 0u, "expected default position");
    zassert_false(reply->isFactory, "expected default isFactory");
    zassert_false(reply->isDownloads, "expected default isDownloads");
    zassert_false(reply->isPlugin, "expected default isPlugin");
    zassert_true(reply->folderKey.empty(), "expected empty folderKey");
    zassert_true(reply->keyInDownloads.empty(), "expected empty keyInDownloads");
    zassert_true(reply->keyInPluginFolder.empty(), "expected empty keyInPluginFolder");
}

ZTEST(catalog_messages, test_decode_setlist_position_empty_input_fails)
{
    zassert_false(decodeSetlistPosition({}).has_value(), "expected nullopt on empty input");
}
