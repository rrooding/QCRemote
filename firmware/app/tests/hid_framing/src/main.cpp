#include <cstdint>
#include <span>
#include <vector>

#include <zephyr/ztest.h>

#include "HidChunker.hpp"
#include "HidReassembler.hpp"

using qcbridge::HidChunker;
using qcbridge::HidReassembler;
using qcbridge::kHidReportSize;

ZTEST_SUITE(hid_framing, NULL, NULL, NULL, NULL, NULL);

namespace {

std::vector<uint8_t> makeReport(uint8_t id, uint8_t len, uint8_t flags,
                                std::span<const uint8_t> payload) {
    std::vector<uint8_t> r(kHidReportSize, 0);
    r[0] = id;
    r[1] = len;
    r[2] = flags;
    for (size_t i = 0; i < payload.size(); ++i) {
        r[3 + i] = payload[i];
    }
    return r;
}

}  // namespace

ZTEST(hid_framing, test_single_report_round_trip)
{
    std::vector<uint8_t> msg = {1, 2, 3, 4, 5};
    HidChunker chunker(msg, 0x02);
    std::vector<uint8_t> report(kHidReportSize);

    zassert_true(chunker.next(report), "expected a report");
    zassert_false(chunker.next(report), "expected done after one report");
    zassert_equal(report[0], 0x02, "report id");
    zassert_equal(report[1], 5, "chunk length");
    zassert_equal(report[2], 0xC0, "flags should be Single");

    HidReassembler<64> reasm;
    auto status = reasm.feed(report);
    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<64>::Status::Complete), "expected Complete");
    auto out = reasm.message();
    zassert_equal(out.size(), msg.size(), "reassembled size");
    zassert_mem_equal(out.data(), msg.data(), msg.size(), "reassembled bytes");
}

ZTEST(hid_framing, test_multi_report_round_trip)
{
    std::vector<uint8_t> msg(300);
    for (size_t i = 0; i < msg.size(); ++i) {
        msg[i] = static_cast<uint8_t>(i);
    }

    HidChunker chunker(msg, 0x02);
    HidReassembler<512> reasm;
    std::vector<uint8_t> report(kHidReportSize);
    int reportCount = 0;
    auto status = HidReassembler<512>::Status::InProgress;

    while (chunker.next(report)) {
        reportCount++;
        status = reasm.feed(report);
    }

    zassert_equal(reportCount, 3, "expected 3 reports for a 300-byte message");
    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<512>::Status::Complete), "expected Complete");
    auto out = reasm.message();
    zassert_equal(out.size(), msg.size(), "reassembled size");
    zassert_mem_equal(out.data(), msg.data(), msg.size(), "reassembled bytes");
}

ZTEST(hid_framing, test_empty_message)
{
    std::vector<uint8_t> msg;
    HidChunker chunker(msg, 0x02);
    std::vector<uint8_t> report(kHidReportSize);

    zassert_true(chunker.next(report), "expected one (empty) report");
    zassert_false(chunker.next(report), "expected done");
    zassert_equal(report[1], 0, "chunk length should be 0");
    zassert_equal(report[2], 0xC0, "flags should be Single");

    HidReassembler<64> reasm;
    auto status = reasm.feed(report);
    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<64>::Status::Complete), "expected Complete");
    zassert_true(reasm.message().empty(), "expected empty message");
}

ZTEST(hid_framing, test_overflow)
{
    std::vector<uint8_t> msg(300);
    HidChunker chunker(msg, 0x02);
    HidReassembler<200> reasm;  // too small for a 300-byte message
    std::vector<uint8_t> report(kHidReportSize);
    auto status = HidReassembler<200>::Status::InProgress;

    while (chunker.next(report)) {
        status = reasm.feed(report);
        if (status == HidReassembler<200>::Status::Overflow) {
            break;
        }
    }

    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<200>::Status::Overflow), "expected Overflow");
}

ZTEST(hid_framing, test_middle_with_no_preceding_first_is_malformed)
{
    std::vector<uint8_t> payload = {9, 9, 9};
    auto report = makeReport(0x01, 3, 0x00 /* Middle */, payload);

    HidReassembler<64> reasm;
    auto status = reasm.feed(report);
    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<64>::Status::Malformed), "expected Malformed");
}

ZTEST(hid_framing, test_bad_chunk_length_is_malformed)
{
    std::vector<uint8_t> payload = {1};
    auto report = makeReport(0x01, 200 /* > 125, invalid */, 0xC0, payload);

    HidReassembler<64> reasm;
    auto status = reasm.feed(report);
    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<64>::Status::Malformed), "expected Malformed");
}

ZTEST(hid_framing, test_wrong_report_size_is_malformed)
{
    std::vector<uint8_t> tooShort(10, 0);

    HidReassembler<64> reasm;
    auto status = reasm.feed(tooShort);
    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<64>::Status::Malformed), "expected Malformed");
}
