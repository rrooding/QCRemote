#include <cstdint>
#include <optional>
#include <vector>

#include <zephyr/ztest.h>

#include "QcHandshakeMessages.hpp"
#include "QcMessage.hpp"
#include "QcProtobuf.hpp"

using namespace qcbridge;

ZTEST_SUITE(protocol_messages, NULL, NULL, NULL, NULL, NULL);

// --- QcProtobuf.hpp ---------------------------------------------------

ZTEST(protocol_messages, test_write_varint_small)
{
    etl::vector<uint8_t, 16> out;
    writeVarint(out, 3);
    const std::vector<uint8_t> expected = {0x03};
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(protocol_messages, test_write_varint_multi_byte)
{
    etl::vector<uint8_t, 16> out;
    writeVarint(out, 300);  // 0b1_0010_1100 -> [0xAC, 0x02]
    const std::vector<uint8_t> expected = {0xAC, 0x02};
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(protocol_messages, test_write_string_field)
{
    etl::vector<uint8_t, 16> out;
    writeString(out, 2, "AB");
    const std::vector<uint8_t> expected = {0x12, 0x02, 'A', 'B'};
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(protocol_messages, test_write_uint64_and_bool_and_enum_fields)
{
    etl::vector<uint8_t, 16> out;
    writeUInt64(out, 1, 0);
    writeBool(out, 2, true);
    writeEnum(out, 1, 3);
    const std::vector<uint8_t> expected = {0x08, 0x00, 0x10, 0x01, 0x08, 0x03};
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(protocol_messages, test_field_reader_round_trip)
{
    etl::vector<uint8_t, 32> buf;
    writeUInt64(buf, 2, 42);
    writeString(buf, 4, "hi");

    ProtoFieldReader reader({buf.data(), buf.size()});

    auto first = reader.next();
    zassert_true(first.has_value(), "expected first field");
    zassert_equal(first->fieldNum, 2u, "field num");
    zassert_true(first->wireType == ProtoWireType::Varint, "wire type");
    zassert_equal(first->varintValue, 42u, "varint value");

    auto second = reader.next();
    zassert_true(second.has_value(), "expected second field");
    zassert_equal(second->fieldNum, 4u, "field num");
    zassert_true(second->wireType == ProtoWireType::LengthDelimited, "wire type");
    zassert_equal(second->bytes.size(), 2u, "bytes size");
    zassert_mem_equal(second->bytes.data(), "hi", 2, "bytes content");

    zassert_false(reader.next().has_value(), "expected end of stream");
}

ZTEST(protocol_messages, test_field_reader_skips_unknown_fields)
{
    etl::vector<uint8_t, 32> buf;
    writeBool(buf, 13, true);       // unrelated field the caller ignores
    writeString(buf, 4, "1.2.3");   // the one the caller wants

    ProtoFieldReader reader({buf.data(), buf.size()});
    std::optional<ProtoFieldReader::Field> wanted;

    while (auto field = reader.next()) {
        if (field->fieldNum == 4) {
            wanted = field;
        }
    }

    zassert_true(wanted.has_value(), "expected to find field 4");
    zassert_equal(wanted->bytes.size(), 5u, "bytes size");
    zassert_mem_equal(wanted->bytes.data(), "1.2.3", 5, "bytes content");
}

ZTEST(protocol_messages, test_field_reader_truncated_varint_is_malformed)
{
    const std::vector<uint8_t> buf = {0x80};  // continuation bit set, nothing follows
    ProtoFieldReader reader({buf.data(), buf.size()});
    zassert_false(reader.next().has_value(), "expected malformed/empty");
}

ZTEST(protocol_messages, test_field_reader_length_past_end_is_malformed)
{
    const std::vector<uint8_t> buf = {0x0A, 0x05, 'a', 'b'};  // tag says 5 bytes, only 2 follow
    ProtoFieldReader reader({buf.data(), buf.size()});
    zassert_false(reader.next().has_value(), "expected malformed");
}

// --- QcMessage.hpp ------------------------------------------------------

ZTEST(protocol_messages, test_encode_decode_message_round_trip)
{
    etl::vector<uint8_t, 32> out;
    const std::vector<uint8_t> proto = {0xAA, 0xBB};
    encodeMessage(out, 0x1234, {proto.data(), proto.size()});

    const std::vector<uint8_t> expected = {0xAA, 0xBB, 0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");

    const auto decoded = decodeMessage({out.data(), out.size()});
    zassert_true(decoded.has_value(), "expected decode to succeed");
    zassert_equal(decoded->command, 0x1234, "command");
    zassert_equal(decoded->hash, 0, "hash");
    zassert_equal(decoded->protoBytes.size(), proto.size(), "proto size");
    zassert_mem_equal(decoded->protoBytes.data(), proto.data(), proto.size(), "proto bytes");
}

ZTEST(protocol_messages, test_decode_message_too_short_fails)
{
    const std::vector<uint8_t> tooShort(7, 0);
    zassert_false(decodeMessage({tooShort.data(), tooShort.size()}).has_value(),
                  "expected decode to fail on < 8 bytes");
}

// --- QcHandshakeMessages.hpp ---------------------------------------------

ZTEST(protocol_messages, test_encode_reset_comms_buffers)
{
    HandshakeMessageBuffer out;
    encodeResetCommsBuffers(out, "AB");

    const std::vector<uint8_t> expected = {
        0x08, 0x00,              // request_id = 0
        0x12, 0x02, 'A', 'B',    // session_id = "AB"
        0x34, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=52
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(protocol_messages, test_encode_version_read)
{
    HandshakeMessageBuffer out;
    encodeVersionRead(out);

    const std::vector<uint8_t> expected = {
        0x08, 0x03,  // action = READ (3)
        0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=10
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(protocol_messages, test_encode_version_update)
{
    HandshakeMessageBuffer out;
    encodeVersionUpdate(out, "AB", 1);

    const std::vector<uint8_t> expected = {
        0x08, 0x01,              // action = UPDATE (1)
        0x10, 0x01,              // request_id = 1
        0x5A, 0x02, 'A', 'B',    // cortex_control_version = "AB" (field 11)
        0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=10
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(protocol_messages, test_encode_connection)
{
    HandshakeMessageBuffer out;
    encodeConnection(out, true, 1);

    const std::vector<uint8_t> expected = {
        0x08, 0x01,  // request_id = 1
        0x10, 0x01,  // connected = true
        0x31, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=49
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(protocol_messages, test_decode_version_reply_picks_out_the_right_fields)
{
    etl::vector<uint8_t, 64> proto;
    writeUInt64(proto, 2, 42);            // request_id - not read
    writeString(proto, 4, "1.2.3");       // zenos_git_hash
    writeBool(proto, 13, true);           // is_ess - not read
    writeString(proto, 7, "buildhash");   // app_fw_version

    const auto reply = decodeVersionReply({proto.data(), proto.size()});
    zassert_true(reply.has_value(), "expected a reply");
    zassert_equal(reply->zenosGitHash.size(), 5u, "zenosGitHash size");
    zassert_mem_equal(reply->zenosGitHash.data(), "1.2.3", 5, "zenosGitHash content");
    zassert_equal(reply->appFwVersion.size(), 9u, "appFwVersion size");
    zassert_mem_equal(reply->appFwVersion.data(), "buildhash", 9, "appFwVersion content");
}

ZTEST(protocol_messages, test_decode_version_reply_missing_fields_yields_empty_strings)
{
    etl::vector<uint8_t, 16> proto;
    writeUInt64(proto, 2, 42);  // only an unrelated field present

    const auto reply = decodeVersionReply({proto.data(), proto.size()});
    zassert_true(reply.has_value(), "expected a reply");
    zassert_true(reply->zenosGitHash.empty(), "expected empty zenosGitHash");
    zassert_true(reply->appFwVersion.empty(), "expected empty appFwVersion");
}

ZTEST(protocol_messages, test_decode_version_reply_empty_input_fails)
{
    zassert_false(decodeVersionReply({}).has_value(), "expected nullopt on empty input");
}
