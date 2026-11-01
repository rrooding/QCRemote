#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <etl/vector.h>

#include "QcCatalogSnapshot.hpp"

namespace qcbridge {

namespace detail {

// Appends `text` to `out` (bounded by `out`'s capacity), escaping the
// handful of characters JSON strings can't contain literally. Preset/folder
// names come from the device's own protocol (QcFileMessage.hpp's
// decodeFile()) and aren't guaranteed free of quotes/backslashes/control
// characters, so this can't just assume plain ASCII and skip escaping.
// Pulled out as a free function so it's unit-testable on its own - see
// tests/preset_transport.
template <size_t N>
inline void appendJsonEscaped(std::string_view text, etl::vector<char, N>& out) {
    for (const char c : text) {
        if (out.full()) {
            return;
        }
        switch (c) {
            case '"':
                if (out.size() + 2 > out.capacity()) return;
                out.push_back('\\');
                out.push_back('"');
                break;
            case '\\':
                if (out.size() + 2 > out.capacity()) return;
                out.push_back('\\');
                out.push_back('\\');
                break;
            case '\n':
                if (out.size() + 2 > out.capacity()) return;
                out.push_back('\\');
                out.push_back('n');
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Any other control character - drop it rather than
                    // emit a \u-escape this encoder doesn't otherwise
                    // support, since none are expected in a real preset
                    // name and the result still has to be valid JSON.
                    break;
                }
                out.push_back(c);
                break;
        }
    }
}

template <size_t N>
inline void appendQuoted(std::string_view text, etl::vector<char, N>& out) {
    if (!out.full()) out.push_back('"');
    appendJsonEscaped(text, out);
    if (!out.full()) out.push_back('"');
}

template <size_t N>
inline void appendLiteral(std::string_view text, etl::vector<char, N>& out) {
    for (const char c : text) {
        if (out.full()) return;
        out.push_back(c);
    }
}

// Decimal, unsigned - every field this encoder prints as a number
// (preset index, folder/preset counts) is non-negative.
template <size_t N>
inline void appendUInt(uint32_t value, etl::vector<char, N>& out) {
    char digits[10];
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0 && !out.full()) {
        out.push_back(digits[--count]);
    }
}

}  // namespace detail

// Encodes a QcCatalogSnapshot as a single JSON object:
// {"generation":N,"presetCount":N,
//  "folders":[{"name":"...","presets":[{"index":N,"name":"..."},...]},...],
//  "incomplete":bool}
//
// Writes into caller-provided, fixed-capacity storage (no heap, per
// CODING_STANDARDS.md). If the snapshot doesn't fit, this stops *before*
// starting whichever folder/preset would have overflowed `out` - checked
// against a generous per-unit + closing-suffix reserve up front, so the
// result is always a complete, valid JSON document (closing brackets and
// all) with "incomplete":true, never a buffer silently cut off mid-string
// or mid-number. Plain C++23 - no Zephyr/Wi-Fi/HTTP dependency, directly
// unit-testable (tests/preset_transport) independent of any real transport.
template <size_t N>
inline void encodeCatalogJson(const QcCatalogSnapshot& snapshot, etl::vector<char, N>& out) {
    out.clear();
    bool truncated = false;

    // Conservative upper bound on the next single unit this function is
    // about to write - either a folder's opening ("{\"name\":\"<=64
    // escaped chars>\",\"presets\":[") or one preset entry
    // ("{\"index\":4294967295,\"name\":\"<=64 escaped chars>\"},") -
    // whichever is bigger, plus this function's own worst-case closing
    // suffix (computed from the literal below, using "false" - one byte
    // longer than "true" - so the reserve covers either outcome). Checked
    // before writing anything for the next folder/preset, so this
    // function only ever stops at a point where it can still close the
    // JSON cleanly. kMaxUnitBytes is deliberately generous (the actual
    // worst case is under 100 bytes even with every name character
    // escaped) rather than computed exactly - overshooting it only costs
    // reserve headroom, never correctness.
    constexpr size_t kMaxUnitBytes = 160;
    constexpr size_t kClosingSuffixBytes = sizeof("]}],\"incomplete\":false}") - 1;
    constexpr size_t kReserve = kMaxUnitBytes + kClosingSuffixBytes;
    const auto roomFor = [&](size_t already) { return already + kReserve <= out.capacity(); };

    detail::appendLiteral("{\"generation\":", out);
    detail::appendUInt(snapshot.generation, out);
    detail::appendLiteral(",\"presetCount\":", out);
    detail::appendUInt(static_cast<uint32_t>(snapshot.presetCount), out);
    detail::appendLiteral(",\"folders\":[", out);

    bool firstFolder = true;
    for (const auto& folder : snapshot.folders) {
        if (!roomFor(out.size())) {
            truncated = true;
            break;
        }
        if (!firstFolder) {
            detail::appendLiteral(",", out);
        }
        firstFolder = false;

        detail::appendLiteral("{\"name\":", out);
        detail::appendQuoted(std::string_view(folder.name.data(), folder.name.size()), out);
        detail::appendLiteral(",\"presets\":[", out);

        bool firstPreset = true;
        for (const auto& preset : folder.presets) {
            if (!roomFor(out.size())) {
                truncated = true;
                break;
            }
            if (!firstPreset) {
                detail::appendLiteral(",", out);
            }
            firstPreset = false;

            detail::appendLiteral("{\"index\":", out);
            detail::appendUInt(preset.index, out);
            detail::appendLiteral(",\"name\":", out);
            detail::appendQuoted(std::string_view(preset.name.data(), preset.name.size()), out);
            detail::appendLiteral("}", out);
        }
        detail::appendLiteral("]}", out);
        if (truncated) {
            break;
        }
    }
    detail::appendLiteral("],\"incomplete\":", out);
    detail::appendLiteral((snapshot.incomplete || truncated) ? "true" : "false", out);
    detail::appendLiteral("}", out);
}

}  // namespace qcbridge
