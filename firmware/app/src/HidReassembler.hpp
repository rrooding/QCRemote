#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <etl/vector.h>

namespace qcbridge {

// qc-mcp's HID report framing (see PROTOCOL.md / transport.py):
//
//   byte 0 : report id (0x01 in / 0x02 out)
//   byte 1 : chunk length (0-126 payload bytes in this report)
//   byte 2 : flags (FIRST/MIDDLE/LAST/SINGLE)
//   byte 3+: payload, zero-padded to kHidReportSize
//
// kHidReportSize is 129, not 128: transport.py's own REPORT_SIZE (128) is the
// *body* (chunk length + flags + payload) and excludes the report-id byte,
// which it prepends separately (message_to_reports() allocates
// `1 + REPORT_SIZE`). kHidReportSize here is the total wire size, report id
// included, so it's REPORT_SIZE + 1.
inline constexpr size_t kHidReportSize = 129;
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
//
// Issue #33's measurement step: a message whose true size exceeds Capacity
// no longer aborts the moment that's detected (the original behavior silently
// dropped the rest of the message, which meant every MIDDLE/LAST report that
// followed for it had no preceding FIRST and miscounted as a second,
// unrelated failure - see Status::Malformed below and the real hardware
// numbers in firmware/README.md's #60/#61 section: overflow=31 but
// malformed=2850, almost all of which were exactly this). Instead it keeps
// consuming reports - without growing the stored body past Capacity - until
// the LAST/SINGLE report completes it, tracking the message's true total
// size and retaining only its last kTrailerTailSize bytes. Those last bytes
// are always QcMessage.hpp's 8-byte trailer ([command][reserved][hash] -
// always the final bytes of a message, appended after the protobuf body),
// so the caller can still learn which command overflowed even though the
// body itself was dropped - see overflowSize()/overflowTrailer() below and
// QcHidBridge.hpp's use of them.
template <size_t Capacity>
class HidReassembler {
public:
    enum class Status {
        InProgress,  // fed a FIRST or MIDDLE report, message isn't complete yet
        Complete,    // fed a LAST or SINGLE report - message() is valid now
        Overflow,    // fed a LAST or SINGLE report, but the true size exceeded
                     // Capacity at some point during assembly - message() is
                     // NOT valid; overflowSize()/overflowTrailer() are instead
        Malformed,   // bad chunk length, or a MIDDLE/LAST with no preceding FIRST
    };

    // Trailer bytes retained across an overflow - sized for QcMessage.hpp's
    // kMessageTrailerSize (not included directly: this file has no
    // dependency on QcMessage.hpp, by design - same reasoning as the rest of
    // this class's plain-C++23, protocol-agnostic framing role). Keep in
    // sync by hand if that trailer size ever changes.
    static constexpr size_t kTrailerTailSize = 8;

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
            overflowing_ = false;
            overflowSize_ = 0;
            tailLen_ = 0;
        } else if (buffer_.empty() && !overflowing_) {
            // A MIDDLE/LAST with no preceding FIRST, and not a continuation
            // of a message already overflowing (see below): out of order,
            // or we missed the FIRST. Nothing sane to reassemble.
            return fail();
        }

        const auto chunk = report.subspan(kHidHeaderSize, chunkLen);

        if (overflowing_) {
            // Already past Capacity for this message - keep tracking its
            // true size and trailing bytes, but don't touch buffer_ (it was
            // cleared, and stays empty, the moment overflow began below).
            overflowSize_ += chunkLen;
            appendTail(chunk);
            if (isLast) {
                overflowing_ = false;
                return Status::Overflow;
            }
            return Status::InProgress;
        }

        if (buffer_.size() + chunkLen > Capacity) {
            // Transition into overflow: the true size so far (whatever was
            // already in buffer_, now folded into overflowSize_/the tail
            // window, plus this chunk) is tracked from here on instead of
            // the body itself.
            overflowing_ = true;
            overflowSize_ = buffer_.size() + chunkLen;
            tailLen_ = 0;
            appendTail({buffer_.data(), buffer_.size()});
            appendTail(chunk);
            buffer_.clear();
            if (isLast) {
                overflowing_ = false;
                return Status::Overflow;
            }
            return Status::InProgress;
        }

        buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());

        return isLast ? Status::Complete : Status::InProgress;
    }

    // Valid only right after feed() returns Complete.
    std::span<const uint8_t> message() const { return {buffer_.data(), buffer_.size()}; }

    // Valid only right after feed() returns Overflow: the message's true
    // total size (report headers excluded), which Capacity could not hold.
    size_t overflowSize() const { return overflowSize_; }

    // Valid only right after feed() returns Overflow: the last (up to)
    // kTrailerTailSize bytes of the message - its trailer, per this class's
    // header comment. Shorter than kTrailerTailSize only if overflowSize()
    // itself is (defensive only - Capacity is always far larger than 8
    // bytes in practice, so overflow never happens that early).
    std::span<const uint8_t> overflowTrailer() const { return {tailBuf_.data(), tailLen_}; }

private:
    Status fail() {
        buffer_.clear();
        overflowing_ = false;
        overflowSize_ = 0;
        tailLen_ = 0;
        return Status::Malformed;
    }

    // Keeps tailBuf_/tailLen_ as a sliding window over the last (up to)
    // kTrailerTailSize bytes fed so far, across however many feed() calls it
    // takes to get there. `chunk` is NOT bounded by kHidMaxPayloadPerReport
    // here: the overflow-transition call site below passes the whole
    // buffer_ accumulated so far (up to Capacity bytes) in one shot, so this
    // must stay correct - and bounded to kTrailerTailSize of stack, not
    // Capacity - for a chunk of any length. It only ever needs the last
    // kTrailerTailSize bytes of (old tail ++ chunk), so when chunk itself is
    // already that long or longer, the old tail is irrelevant and dropped;
    // otherwise just enough of the old tail survives to pad chunk out to the
    // window size.
    void appendTail(std::span<const uint8_t> chunk) {
        if (chunk.size() >= kTrailerTailSize) {
            const uint8_t* tail = chunk.data() + (chunk.size() - kTrailerTailSize);
            for (size_t i = 0; i < kTrailerTailSize; ++i) {
                tailBuf_[i] = tail[i];
            }
            tailLen_ = kTrailerTailSize;
            return;
        }

        const size_t keepFromOld = kTrailerTailSize - chunk.size();
        const size_t oldKeep = tailLen_ < keepFromOld ? tailLen_ : keepFromOld;
        const size_t oldStart = tailLen_ - oldKeep;
        std::array<uint8_t, kTrailerTailSize> next{};
        size_t n = 0;
        for (size_t i = oldStart; i < tailLen_; ++i) {
            next[n++] = tailBuf_[i];
        }
        for (const uint8_t byte : chunk) {
            next[n++] = byte;
        }
        for (size_t i = 0; i < n; ++i) {
            tailBuf_[i] = next[i];
        }
        tailLen_ = n;
    }

    etl::vector<uint8_t, Capacity> buffer_;
    bool overflowing_ = false;
    size_t overflowSize_ = 0;
    std::array<uint8_t, kTrailerTailSize> tailBuf_{};
    size_t tailLen_ = 0;
};

}  // namespace qcbridge
