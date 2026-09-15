#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "HidReassembler.hpp"

namespace qcbridge {

// Splits a logical message into a sequence of 128-byte HID reports, per
// qc-mcp's framing (see HidReassembler.hpp for the byte layout this
// mirrors). Call next() repeatedly until done() - each call fills exactly
// one report.
class HidChunker {
public:
    HidChunker(std::span<const uint8_t> message, uint8_t reportId)
        : message_(message), reportId_(reportId) {}

    bool done() const { return emittedAtLeastOne_ && offset_ >= message_.size(); }

    // Fills `outReport` (must be exactly kHidReportSize bytes) with the next
    // report. Returns false if done() was already true (nothing written).
    bool next(std::span<uint8_t> outReport) {
        if (outReport.size() != kHidReportSize || done()) {
            return false;
        }

        const size_t remaining = message_.size() - offset_;
        const size_t chunkLen =
            remaining < kHidMaxPayloadPerReport ? remaining : kHidMaxPayloadPerReport;
        const bool isFirst = offset_ == 0;
        const bool isLast = offset_ + chunkLen >= message_.size();

        HidReportFlag flag;
        if (isFirst && isLast) {
            flag = HidReportFlag::Single;
        } else if (isFirst) {
            flag = HidReportFlag::First;
        } else if (isLast) {
            flag = HidReportFlag::Last;
        } else {
            flag = HidReportFlag::Middle;
        }

        outReport[0] = reportId_;
        outReport[1] = static_cast<uint8_t>(chunkLen);
        outReport[2] = static_cast<uint8_t>(flag);
        for (size_t i = 0; i < kHidMaxPayloadPerReport; ++i) {
            outReport[kHidHeaderSize + i] = i < chunkLen ? message_[offset_ + i] : uint8_t{0};
        }

        offset_ += chunkLen;
        emittedAtLeastOne_ = true;
        return true;
    }

private:
    std::span<const uint8_t> message_;
    uint8_t reportId_;
    size_t offset_ = 0;
    bool emittedAtLeastOne_ = false;
};

}  // namespace qcbridge
