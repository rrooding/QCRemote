#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <etl/vector.h>

namespace qcbridge {

// qc-mcp's HID report framing (see PROTOCOL.md / transport.py):
//
//   byte 0 : report id (0x01 in / 0x02 out)
//   byte 1 : chunk length (0-125 payload bytes in this report)
//   byte 2 : flags (FIRST/MIDDLE/LAST/SINGLE)
//   byte 3+: payload, zero-padded to kHidReportSize
inline constexpr size_t kHidReportSize = 128;
inline constexpr size_t kHidHeaderSize = 3;
inline constexpr size_t kHidMaxPayloadPerReport = kHidReportSize - kHidHeaderSize;

enum class HidReportFlag : uint8_t {
    Middle = 0x00,
    First = 0x40,
    Last = 0x80,
    Single = 0xC0,
};

// Reassembles a sequence of 128-byte HID reports into one logical message.
//
// Capacity is the largest reassembled message this instance can hold - see
// issue #33 for the actual sizing exercise. Not yet instantiated anywhere
// in the real transport (that's the still-open part of #4 - see usbh_shim.c);
// this file and its Ztest suite (tests/hid_framing) stand on their own.
template <size_t Capacity>
class HidReassembler {
public:
    enum class Status {
        InProgress,  // fed a FIRST or MIDDLE report, message isn't complete yet
        Complete,    // fed a LAST or SINGLE report - message() is valid now
        Overflow,    // message would exceed Capacity; reset, nothing retained
        Malformed,   // bad chunk length, or a MIDDLE/LAST with no preceding FIRST
    };

    // Feeds one report, exactly kHidReportSize bytes.
    Status feed(std::span<const uint8_t> report) {
        if (report.size() != kHidReportSize) {
            return fail();
        }

        const uint8_t chunkLen = report[1];
        const uint8_t flags = report[2];

        if (chunkLen > kHidMaxPayloadPerReport) {
            return fail();
        }

        const bool isFirst = flags == static_cast<uint8_t>(HidReportFlag::First) ||
                             flags == static_cast<uint8_t>(HidReportFlag::Single);
        const bool isMiddle = flags == static_cast<uint8_t>(HidReportFlag::Middle);
        const bool isLast = flags == static_cast<uint8_t>(HidReportFlag::Last) ||
                            flags == static_cast<uint8_t>(HidReportFlag::Single);

        if (!isFirst && !isMiddle && !isLast) {
            return fail();
        }

        if (isFirst) {
            buffer_.clear();
        } else if (buffer_.empty()) {
            // A MIDDLE/LAST with no preceding FIRST: out of order, or we
            // missed the FIRST. Nothing sane to reassemble.
            return fail();
        }

        if (buffer_.size() + chunkLen > Capacity) {
            buffer_.clear();
            return Status::Overflow;
        }

        buffer_.insert(buffer_.end(), report.begin() + kHidHeaderSize,
                       report.begin() + kHidHeaderSize + chunkLen);

        return isLast ? Status::Complete : Status::InProgress;
    }

    // Valid only right after feed() returns Complete.
    std::span<const uint8_t> message() const { return {buffer_.data(), buffer_.size()}; }

private:
    Status fail() {
        buffer_.clear();
        return Status::Malformed;
    }

    etl::vector<uint8_t, Capacity> buffer_;
};

}  // namespace qcbridge
