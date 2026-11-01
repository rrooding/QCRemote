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
    // 300 bytes, too small for a 200-byte Capacity.
    std::vector<uint8_t> msg(300, 0xAB);
    HidChunker chunker(msg, 0x02);
    HidReassembler<200> reasm;  // too small for a 300-byte message
    std::vector<uint8_t> report(kHidReportSize);
    auto status = HidReassembler<200>::Status::InProgress;
    int reportCount = 0;

    // issue #33: feed every report through to the end (no early break on
    // the first Overflow-ish report) - Overflow is only returned once the
    // LAST/SINGLE report completes the message, not the moment Capacity is
    // first exceeded.
    while (chunker.next(report)) {
        ++reportCount;
        status = reasm.feed(report);
        if (reportCount < 3) {
            zassert_equal(static_cast<int>(status), static_cast<int>(HidReassembler<200>::Status::InProgress),
                          "expected InProgress while still mid-message, even past Capacity");
        }
    }

    zassert_equal(reportCount, 3, "expected 3 reports for a 300-byte message");
    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<200>::Status::Overflow), "expected Overflow");
    zassert_equal(reasm.overflowSize(), msg.size(), "overflowSize should be the true total size");
}

ZTEST(hid_framing, test_overflow_size_correct_with_large_buffered_body_before_transition)
{
    // The overflow-transition branch computes overflowSize_ from
    // buffer_.size() + the new report's chunk, not just the chunk alone -
    // must stay correct once a couple of reports have already accumulated
    // (252 buffered bytes here, from two full 126-byte reports) before the
    // third report's transition pushes the message over Capacity.
    std::vector<uint8_t> msg(352, 0x77);
    HidChunker chunker(msg, 0x02);
    HidReassembler<300> reasm;
    std::vector<uint8_t> report(kHidReportSize);
    auto status = HidReassembler<300>::Status::InProgress;
    int reportCount = 0;

    while (chunker.next(report)) {
        ++reportCount;
        status = reasm.feed(report);
    }

    zassert_equal(reportCount, 3, "expected 3 reports for a 352-byte message");
    zassert_equal(static_cast<int>(status),
                  static_cast<int>(HidReassembler<300>::Status::Overflow), "expected Overflow");
    zassert_equal(reasm.overflowSize(), msg.size(), "overflowSize should be the true total size");
}

ZTEST(hid_framing, test_overflow_then_resync_on_next_first)
{
    std::vector<uint8_t> big(300, 0xCD);
    HidChunker bigChunker(big, 0x02);
    HidReassembler<200> reasm;
    std::vector<uint8_t> report(kHidReportSize);
    auto status = HidReassembler<200>::Status::InProgress;

    while (bigChunker.next(report)) {
        status = reasm.feed(report);
    }
    zassert_equal(static_cast<int>(status), static_cast<int>(HidReassembler<200>::Status::Overflow),
                  "expected the big message to overflow first");

    // A fresh FIRST/SINGLE for an unrelated, small message must reassemble
    // normally - overflow state from the previous message must not leak.
    std::vector<uint8_t> small = {1, 2, 3, 4, 5};
    HidChunker smallChunker(small, 0x02);
    zassert_true(smallChunker.next(report), "expected one report for a 5-byte message");
    status = reasm.feed(report);

    zassert_equal(static_cast<int>(status), static_cast<int>(HidReassembler<200>::Status::Complete),
                  "expected Complete for the small message after a resync");
    const auto out = reasm.message();
    zassert_equal(out.size(), small.size(), "reassembled size");
    zassert_mem_equal(out.data(), small.data(), small.size(), "reassembled bytes");
}

ZTEST(hid_framing, test_malformed_report_while_overflowing_resets_cleanly)
{
    std::vector<uint8_t> big(300, 0xEF);
    HidChunker bigChunker(big, 0x02);
    HidReassembler<200> reasm;
    std::vector<uint8_t> report(kHidReportSize);

    // First report: within Capacity, InProgress.
    zassert_true(bigChunker.next(report), "expected a first report");
    zassert_equal(static_cast<int>(reasm.feed(report)),
                  static_cast<int>(HidReassembler<200>::Status::InProgress), "expected InProgress");

    // Second report pushes it over Capacity (still not the last one) -
    // now mid-overflow.
    zassert_true(bigChunker.next(report), "expected a second report");
    zassert_equal(static_cast<int>(reasm.feed(report)),
                  static_cast<int>(HidReassembler<200>::Status::InProgress),
                  "expected InProgress while overflowing but not yet complete");

    // A malformed report (bad chunk length) while mid-overflow must still
    // reset cleanly, the same as it would mid-InProgress.
    std::vector<uint8_t> badPayload = {1};
    auto badReport = makeReport(0x01, 200 /* > kHidMaxPayloadPerReport */, 0xC0, badPayload);
    zassert_equal(static_cast<int>(reasm.feed(badReport)),
                  static_cast<int>(HidReassembler<200>::Status::Malformed), "expected Malformed");

    // And a brand-new message afterward must reassemble normally.
    std::vector<uint8_t> small = {9, 8, 7};
    HidChunker smallChunker(small, 0x02);
    std::vector<uint8_t> smallReport(kHidReportSize);
    zassert_true(smallChunker.next(smallReport), "expected one report for a 3-byte message");
    const auto status = reasm.feed(smallReport);
    zassert_equal(static_cast<int>(status), static_cast<int>(HidReassembler<200>::Status::Complete),
                  "expected Complete after resetting from a malformed report mid-overflow");
}

ZTEST(hid_framing, test_continuation_reports_after_overflow_are_not_malformed)
{
    // This is the real-hardware symptom (issue #60/#61: overflow=31,
    // malformed=2850) the "keep going" behavior fixes: before issue #33,
    // Overflow reset the reassembler immediately, so every MIDDLE/LAST
    // report that followed for the same message had no preceding FIRST and
    // was miscounted as a second, unrelated Malformed. Now they're
    // legitimately InProgress/Overflow instead.
    std::vector<uint8_t> big(400, 0x11);
    HidChunker chunker(big, 0x02);
    HidReassembler<200> reasm;
    std::vector<uint8_t> report(kHidReportSize);
    auto status = HidReassembler<200>::Status::InProgress;

    while (chunker.next(report)) {
        status = reasm.feed(report);
        zassert_true(status == HidReassembler<200>::Status::InProgress ||
                         status == HidReassembler<200>::Status::Overflow,
                     "no continuation report after an overflow should ever be Malformed");
    }
    zassert_equal(static_cast<int>(status), static_cast<int>(HidReassembler<200>::Status::Overflow),
                  "expected the message to finish as Overflow");
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
    auto report = makeReport(0x01, 200 /* > kHidMaxPayloadPerReport (126), invalid */, 0xC0, payload);

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
