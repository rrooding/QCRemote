#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>

#include "QcFileMessage.hpp"
#include "QcHandshakeMessages.hpp"  // MessageAction
#include "QcProtobuf.hpp"

using namespace qcbridge;

ZTEST_SUITE(file_messages, NULL, NULL, NULL, NULL, NULL);

namespace {

template <typename Container>
std::string_view asStringView(const Container& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// One ProductData entry: field 3 = name. Fields 1 (key)/2 (index) are left
// out deliberately - issue #59's decoder doesn't read them, matching the
// "fields not needed here" scope from the issue body.
etl::vector<uint8_t, 512> buildProduct(std::string_view name) {
    etl::vector<uint8_t, 512> out;
    writeString(out, 3, name);
    return out;
}

// A FolderInfo: field 1 = key, 3 = name, 4 = is_factory, 7 = files
// (repeated - one writeString(..., 7, ...) call per ProductData).
etl::vector<uint8_t, 2048> buildFolder(std::string_view key, std::string_view name, bool isFactory,
                                        const std::vector<std::string_view>& presetNames) {
    etl::vector<uint8_t, 2048> out;
    writeString(out, 1, key);
    writeString(out, 3, name);
    writeBool(out, 4, isFactory);
    for (const auto& presetName : presetNames) {
        const auto product = buildProduct(presetName);
        writeString(out, 7, asStringView(product));
    }
    return out;
}

}  // namespace

// --- happy paths ---------------------------------------------------------

ZTEST(file_messages, test_decode_file_single_folder_multiple_presets)
{
    const auto folder = buildFolder("1_q", "My Presets", false, {"Lead Tone", "Clean Rhythm"});

    etl::vector<uint8_t, 2048> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Update));
    writeUInt64(proto, 3, kFileTypePresets);
    writeString(proto, 4, asStringView(folder));

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_equal(presets.size(), 2u, "expected two presets");

    zassert_equal(presets[0].folderKey, "1_q", "folderKey");
    zassert_equal(presets[0].folderName, "My Presets", "folderName");
    zassert_false(presets[0].folderIsFactory, "folderIsFactory");
    zassert_equal(presets[0].presetName, "Lead Tone", "presetName");
    zassert_equal(presets[0].index, 0u, "index");

    zassert_equal(presets[1].folderKey, "1_q", "folderKey");
    zassert_equal(presets[1].presetName, "Clean Rhythm", "presetName");
    zassert_equal(presets[1].index, 1u, "index");
}

ZTEST(file_messages, test_decode_file_multiple_folders_are_independent)
{
    const auto userFolder = buildFolder("1_q", "My Presets", false, {"Lead Tone"});
    const auto factoryFolder = buildFolder("/opt/neuraldsp/factory", "Factory Library", true,
                                            {"Default Clean"});

    etl::vector<uint8_t, 2048> userProto;
    writeUInt64(userProto, 3, kFileTypePresets);
    writeString(userProto, 4, asStringView(userFolder));

    etl::vector<uint8_t, 2048> factoryProto;
    writeUInt64(factoryProto, 3, kFileTypePresets);
    writeString(factoryProto, 4, asStringView(factoryFolder));

    std::vector<FilePreset> presets;
    auto collect = [&](const FilePreset& preset) { presets.push_back(preset); };

    zassert_true(decodeFile({userProto.data(), userProto.size()}, collect), "user folder decode");
    zassert_true(decodeFile({factoryProto.data(), factoryProto.size()}, collect), "factory folder decode");

    zassert_equal(presets.size(), 2u, "expected one preset per folder");
    zassert_equal(presets[0].folderKey, "1_q", "first folderKey");
    zassert_false(presets[0].folderIsFactory, "first folderIsFactory");
    zassert_equal(presets[1].folderKey, "/opt/neuraldsp/factory", "second folderKey");
    zassert_true(presets[1].folderIsFactory, "second folderIsFactory");
}

ZTEST(file_messages, test_decode_file_long_preset_and_folder_names)
{
    const std::string longFolderName(200, 'f');
    const std::string longPresetName(300, 'p');
    const auto folder = buildFolder("1_q", longFolderName, false, {longPresetName});

    etl::vector<uint8_t, 2048> proto;
    writeUInt64(proto, 3, kFileTypePresets);
    writeString(proto, 4, asStringView(folder));

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_equal(presets.size(), 1u, "expected one preset");
    zassert_equal(presets[0].folderName, longFolderName, "folderName");
    zassert_equal(presets[0].presetName, longPresetName, "presetName");
}

ZTEST(file_messages, test_decode_file_skips_unknown_fields_at_every_level)
{
    etl::vector<uint8_t, 512> product;
    writeString(product, 1, "key-ignored");    // unknown-to-decoder field, before the one read
    writeString(product, 3, "Lead Tone");
    writeBool(product, 19, true);               // is_readonly - not read, comes after

    etl::vector<uint8_t, 512> folder;
    writeString(folder, 2, "parent-ignored");   // parent_key - not read
    writeString(folder, 1, "1_q");
    writeString(folder, 3, "My Presets");
    writeBool(folder, 4, false);
    writeString(folder, 7, asStringView(product));
    writeBool(folder, 9, true);                  // is_favorites - not read

    etl::vector<uint8_t, 512> proto;
    writeUInt64(proto, 2, 7);                    // request_id - not read
    writeUInt64(proto, 3, kFileTypePresets);
    writeString(proto, 4, asStringView(folder));
    writeBool(proto, 6, true);                   // short_listing - not read

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_equal(presets.size(), 1u, "expected one preset");
    zassert_equal(presets[0].folderKey, "1_q", "folderKey");
    zassert_equal(presets[0].presetName, "Lead Tone", "presetName");
}

// --- non-preset / absent-data paths --------------------------------------

ZTEST(file_messages, test_decode_file_irs_type_yields_no_presets)
{
    const auto folder = buildFolder("CIR_abc", "My IRs", false, {"Cab IR"});

    etl::vector<uint8_t, 512> proto;
    writeUInt64(proto, 3, 1);  // type = 1 (IRs)
    writeString(proto, 4, asStringView(folder));

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_true(presets.empty(), "expected no presets for an IRs folder");
}

ZTEST(file_messages, test_decode_file_captures_type_yields_no_presets)
{
    const auto folder = buildFolder("deadbeef", "My Captures", false, {"Amp Capture"});

    etl::vector<uint8_t, 512> proto;
    writeUInt64(proto, 3, 2);  // type = 2 (Captures)
    writeString(proto, 4, asStringView(folder));

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_true(presets.empty(), "expected no presets for a captures folder");
}

ZTEST(file_messages, test_decode_file_missing_type_yields_no_presets)
{
    const auto folder = buildFolder("1_q", "My Presets", false, {"Lead Tone"});

    etl::vector<uint8_t, 512> proto;
    writeString(proto, 4, asStringView(folder));  // no type field at all

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_true(presets.empty(), "expected no presets without a type field");
}

ZTEST(file_messages, test_decode_file_missing_folder_yields_no_presets)
{
    etl::vector<uint8_t, 16> proto;
    writeUInt64(proto, 3, kFileTypePresets);  // type present, folder absent

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_true(presets.empty(), "expected no presets without a folder field");
}

ZTEST(file_messages, test_decode_file_folder_with_no_files_yields_no_presets)
{
    const auto folder = buildFolder("1_q", "My Presets", false, {});

    etl::vector<uint8_t, 512> proto;
    writeUInt64(proto, 3, kFileTypePresets);
    writeString(proto, 4, asStringView(folder));

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_true(presets.empty(), "expected no presets for an empty folder");
}

// --- malformed / truncated input -----------------------------------------

ZTEST(file_messages, test_decode_file_empty_input_fails)
{
    std::vector<FilePreset> presets;
    const bool ok = decodeFile({}, [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_false(ok, "expected decode to fail on empty input");
    zassert_true(presets.empty(), "expected no presets");
}

ZTEST(file_messages, test_decode_file_truncated_varint_skips_cleanly)
{
    const std::vector<uint8_t> proto = {0x18, 0x80};  // field 3 tag, varint continuation with nothing after

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "truncated top-level input is not itself a failure");
    zassert_true(presets.empty(), "expected no presets");
}

ZTEST(file_messages, test_decode_file_folder_length_past_end_skips_cleanly)
{
    etl::vector<uint8_t, 16> proto;
    writeUInt64(proto, 3, kFileTypePresets);
    writeTag(proto, 4, ProtoWireType::LengthDelimited);
    writeVarint(proto, 100);  // claims 100 bytes of folder; none follow

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "a malformed folder length is not itself a failure");
    zassert_true(presets.empty(), "expected no presets - the folder field never parsed");
}

ZTEST(file_messages, test_decode_file_one_malformed_product_does_not_stop_valid_ones)
{
    etl::vector<uint8_t, 512> folder;
    writeString(folder, 1, "1_q");
    writeString(folder, 3, "My Presets");
    writeBool(folder, 4, false);

    const auto validProduct = buildProduct("Lead Tone");
    writeString(folder, 7, asStringView(validProduct));

    // A second "files" occurrence whose declared length runs past the
    // folder's own bytes - must be skipped, not read out of bounds, and
    // must not disturb the valid entry already captured above.
    writeTag(folder, 7, ProtoWireType::LengthDelimited);
    writeVarint(folder, 50);

    etl::vector<uint8_t, 512> proto;
    writeUInt64(proto, 3, kFileTypePresets);
    writeString(proto, 4, asStringView(folder));

    std::vector<FilePreset> presets;
    const bool ok = decodeFile({proto.data(), proto.size()},
                                [&](const FilePreset& preset) { presets.push_back(preset); });

    zassert_true(ok, "expected decode to succeed");
    zassert_equal(presets.size(), 1u, "expected only the valid preset");
    zassert_equal(presets[0].presetName, "Lead Tone", "presetName");
}
