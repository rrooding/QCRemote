#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

#include <miniz.h>

namespace qcbridge::gzip {

// RFC 1952 member magic + deflate compression method. qc-mcp's own
// decode_message() (protocol.py) checks exactly these 3 bytes at the
// *start* of the protobuf blob - not a scan for the magic anywhere in the
// buffer, which is what its separate gunzip() helper does for a different
// call site. Matched here deliberately: a command whose plain protobuf
// encoding happens to contain "\x1f\x8b\x08" somewhere past byte 0 must
// NOT be treated as compressed. See ADR 0005.
inline constexpr std::array<uint8_t, 3> kMagic = {0x1f, 0x8b, 0x08};

inline bool hasMagic(std::span<const uint8_t> data) {
    return data.size() >= kMagic.size() && std::equal(kMagic.begin(), kMagic.end(), data.begin());
}

enum class InflateError {
    NotGzipped,       // hasMagic() was false - caller shouldn't have called inflate()
    HeaderMalformed,  // gzip header had reserved flag bits set, or an optional field ran past the input
    Truncated,        // the deflate stream (or its 8-byte trailer) ran out before completing
    OutputTooLarge,   // decompressed data didn't fit in `output`
    DecompressFailed, // tinfl reported a corrupt deflate stream
    CrcMismatch,      // decompressed fully, but the trailer's CRC32/length didn't match
};

// For logging (QcGzipInflater.hpp) - so the first real compressed message
// that fails to inflate shows up as, say, "CrcMismatch" rather than an
// opaque integer. Not used for control flow anywhere, so a missing case
// just falls through to the default rather than needing exhaustiveness.
inline const char* toString(InflateError error) {
    switch (error) {
    case InflateError::NotGzipped:
        return "NotGzipped";
    case InflateError::HeaderMalformed:
        return "HeaderMalformed";
    case InflateError::Truncated:
        return "Truncated";
    case InflateError::OutputTooLarge:
        return "OutputTooLarge";
    case InflateError::DecompressFailed:
        return "DecompressFailed";
    case InflateError::CrcMismatch:
        return "CrcMismatch";
    default:
        return "Unknown";
    }
}

// Upper bound on the *compressed* gzip member this module will accept.
// Matches QcHidBridge.hpp's kReassemblyCapacity (today's issue #33
// placeholder, 4096 bytes) - a message's protobuf blob can never be
// larger than the whole reassembled HID message it came from, so this
// can't usefully be bigger than that without #33 changing first. Not an
// #include of that header: QcHidBridge.hpp already includes QcSession.hpp
// (which pulls this one in), so including it back would be circular.
// Keep the two values in sync by hand if #33 revisits kReassemblyCapacity.
inline constexpr size_t kMaxCompressedInputSize = 4096;

// Upper bound on the *decompressed* output this module will produce.
// 16 KiB = 4x kMaxCompressedInputSize above: generous headroom for the
// gzip ratios (commonly 3-6x) typical of the text/protobuf payloads this
// protocol carries over HID, while still bounding a maliciously-crafted
// "zip bomb" style input (DEFLATE can in principle expand a small
// compressed stream by orders of magnitude via back-reference abuse) to a
// fixed, clean failure (OutputTooLarge) rather than an unbounded
// allocation. This is independent of - and much smaller than -
// ModelRepo's *internal* catalog payload (a separate gzip+tar of
// ModelRepo.xml inside a protobuf bytes field, confirmed ~675KB
// uncompressed from qc-mcp's own fixture): this module does not attempt
// to hold that. Untarring/decompressing the catalog itself is explicitly
// out of scope for this issue (see the issue's scope notes) and would
// need #33's broader memory-budget exercise - and probably streaming
// decompression - to support regardless of this constant.
inline constexpr size_t kMaxInflatedSize = 16384;

namespace detail {

// Standard reflected CRC-32 (the one gzip/zlib/PNG all use, matching
// Python's zlib.crc32()/binascii.crc32()) - hand-rolled instead of
// miniz's mz_crc32() so this module doesn't need to compile/link miniz.c
// (which also pulls in the deflate/zip-archive code we don't want) just
// for one checksum. Bit-at-a-time rather than a 256-entry lookup table:
// this only ever runs over at most kMaxInflatedSize bytes, on a
// background thread (QcGzipInflater), not a hot path - trading a little
// CPU for skipping a 1KB table is the right side of that tradeoff here.
inline uint32_t crc32(std::span<const uint8_t> data) {
    uint32_t crc = 0xFFFFFFFFu;
    for (const uint8_t byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            const uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

inline uint32_t readLE32(std::span<const uint8_t, 4> bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

}  // namespace detail

// Inflates a gzip member (RFC 1952) whose magic byte prefix has already
// been confirmed by hasMagic(). Strict: requires the full gzip trailer to
// be present and validates both its CRC32 and its length field against
// the decompressed output. qc-mcp's own gunzip() helper (protocol.py)
// falls back to a non-validating streaming decompress when its first,
// strict attempt raises - that fallback is deliberately NOT reproduced
// here. See ADR 0005 for why: a firmware decoder failing closed on
// malformed/truncated/corrupt input is preferable to one that silently
// hands a caller partially-decompressed bytes it has no way to tell apart
// from a real, complete payload.
//
// Never allocates and never recurses. Its one large piece of working
// state - tinfl's own ~8KB Huffman-table struct - lives in a
// function-local `static` rather than on the caller's stack (it's a
// trivial/POD type, so this is ordinary zero-initialized static storage,
// not a dynamically-initialized "magic static" - no runtime guard, no
// threading machinery involved). That static is NOT safe for concurrent
// calls from more than one thread; this module has exactly one real
// caller (QcGzipInflater), which only ever calls it from its own single
// dedicated thread - don't add a second caller without reconsidering
// this.
//
// Must NOT be called from Zephyr's `usbh` thread (QcHidBridge.hpp /
// QcSession.hpp) - see QcGzipInflater's header comment for why.
inline std::expected<std::span<const uint8_t>, InflateError> inflate(std::span<const uint8_t> gzipped,
                                                                       std::span<uint8_t> output) {
    if (!hasMagic(gzipped)) {
        return std::unexpected(InflateError::NotGzipped);
    }

    constexpr size_t kFixedHeaderSize = 10;
    constexpr size_t kTrailerSize = 8;

    // Checked against the flags byte (offset 3) before the full
    // fixed-header/trailer length check below, deliberately: a header
    // with reserved bits set is malformed regardless of how long (or
    // short) the rest of the buffer is, and callers reporting "which gzip
    // member did this come from" want that distinguished from a plain
    // truncation.
    constexpr uint8_t kFlagHeaderCrc = 0x02;
    constexpr uint8_t kFlagExtra = 0x04;
    constexpr uint8_t kFlagName = 0x08;
    constexpr uint8_t kFlagComment = 0x10;
    constexpr uint8_t kFlagReservedMask = 0xE0;

    constexpr size_t kFlagsByteOffset = 3;
    if (gzipped.size() <= kFlagsByteOffset) {
        return std::unexpected(InflateError::Truncated);
    }
    const uint8_t flags = gzipped[kFlagsByteOffset];
    if ((flags & kFlagReservedMask) != 0) {
        return std::unexpected(InflateError::HeaderMalformed);
    }

    if (gzipped.size() < kFixedHeaderSize + kTrailerSize) {
        return std::unexpected(InflateError::Truncated);
    }

    size_t offset = kFixedHeaderSize;

    if (flags & kFlagExtra) {
        if (offset + 2 > gzipped.size()) {
            return std::unexpected(InflateError::Truncated);
        }
        const uint16_t extraLen =
            static_cast<uint16_t>(gzipped[offset]) | static_cast<uint16_t>(gzipped[offset + 1] << 8);
        offset += 2 + extraLen;
        if (offset > gzipped.size()) {
            return std::unexpected(InflateError::Truncated);
        }
    }
    if (flags & kFlagName) {
        while (true) {
            if (offset >= gzipped.size()) {
                return std::unexpected(InflateError::Truncated);
            }
            if (gzipped[offset++] == 0) {
                break;
            }
        }
    }
    if (flags & kFlagComment) {
        while (true) {
            if (offset >= gzipped.size()) {
                return std::unexpected(InflateError::Truncated);
            }
            if (gzipped[offset++] == 0) {
                break;
            }
        }
    }
    if (flags & kFlagHeaderCrc) {
        offset += 2;
        if (offset > gzipped.size()) {
            return std::unexpected(InflateError::Truncated);
        }
    }

    if (offset + kTrailerSize > gzipped.size()) {
        return std::unexpected(InflateError::Truncated);
    }

    const std::span<const uint8_t> deflateAndTrailer = gzipped.subspan(offset);

    static tinfl_decompressor decompressor{};
    tinfl_init(&decompressor);

    size_t inAvail = deflateAndTrailer.size();
    size_t outAvail = output.size();

    const tinfl_status status =
        tinfl_decompress(&decompressor, deflateAndTrailer.data(), &inAvail, output.data(), output.data(),
                          &outAvail, TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);

    switch (status) {
    case TINFL_STATUS_DONE:
        break;
    case TINFL_STATUS_HAS_MORE_OUTPUT:
        return std::unexpected(InflateError::OutputTooLarge);
    case TINFL_STATUS_NEEDS_MORE_INPUT:
        return std::unexpected(InflateError::Truncated);
    default:
        return std::unexpected(InflateError::DecompressFailed);
    }

    const size_t decompressedSize = outAvail;    // bytes tinfl wrote this call
    const size_t consumedCompressed = inAvail;    // bytes tinfl consumed this call
    const size_t trailerRemaining = deflateAndTrailer.size() - consumedCompressed;
    if (trailerRemaining != kTrailerSize) {
        // Either the deflate stream's final block hasn't been reached yet
        // (less than kTrailerSize left - truncated), or there's trailing
        // data beyond a single trailer (more than kTrailerSize left -
        // not a format this function supports). Both are clean failures.
        return std::unexpected(InflateError::Truncated);
    }

    const std::span<const uint8_t> trailer = deflateAndTrailer.subspan(consumedCompressed, kTrailerSize);
    const uint32_t expectedCrc = detail::readLE32(trailer.subspan<0, 4>());
    const uint32_t expectedSize = detail::readLE32(trailer.subspan<4, 4>());

    const std::span<const uint8_t> decompressed = output.subspan(0, decompressedSize);
    if (expectedSize != static_cast<uint32_t>(decompressedSize) ||
        expectedCrc != detail::crc32(decompressed)) {
        return std::unexpected(InflateError::CrcMismatch);
    }

    return decompressed;
}

// Reads the gzip trailer's ISIZE field (RFC 1952: the last 4 of the
// member's last 8 bytes, little-endian, the uncompressed size modulo 2^32)
// directly from the compressed bytes, without running tinfl at all. The
// trailer sits at a fixed offset from the *end* of a gzip member
// regardless of header length (FEXTRA/FNAME/FCOMMENT don't move it), so
// this works even when inflate() above fails with OutputTooLarge - issue
// #33's measurement needs the true decompressed size of a message that's
// too big for `output` to size a buffer for real, and this is the cheap
// way to get it without decompressing anything. Real hardware hit exactly
// this case: a `RecallPreset` whose compressed size (2408 bytes) implied
// an inflated size past kMaxInflatedSize.
//
// std::nullopt if `gzipped` is too short to even hold a trailer - callers
// already holding a hasMagic()'d, reassembled message (the only way
// inflate() gets called at all) always have enough bytes here in
// practice. Doesn't itself check hasMagic()/the header - a caller already
// knows that by construction (same contract as inflate() above).
inline std::optional<uint32_t> peekDecompressedSize(std::span<const uint8_t> gzipped) {
    constexpr size_t kTrailerSize = 8;
    if (gzipped.size() < kTrailerSize) {
        return std::nullopt;
    }
    const auto trailer = gzipped.last(kTrailerSize);
    return detail::readLE32(trailer.subspan<4, 4>());
}

}  // namespace qcbridge::gzip
