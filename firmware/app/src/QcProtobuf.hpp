#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <etl/vector.h>

namespace qcbridge {

// Minimal protobuf wire-format primitives, scoped to the flat scalar-field
// messages issue #7's handshake needs (see ADR 0003 for why this exists
// instead of nanopb). No nested messages, no repeated/packed fields.
enum class ProtoWireType : uint8_t {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    Fixed32 = 5,
};

inline void writeVarint(etl::ivector<uint8_t>& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>(value) | 0x80);
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

inline void writeTag(etl::ivector<uint8_t>& out, uint32_t fieldNum, ProtoWireType wireType) {
    writeVarint(out, (static_cast<uint64_t>(fieldNum) << 3) | static_cast<uint64_t>(wireType));
}

inline void writeString(etl::ivector<uint8_t>& out, uint32_t fieldNum, std::string_view value) {
    writeTag(out, fieldNum, ProtoWireType::LengthDelimited);
    writeVarint(out, value.size());
    for (const char c : value) {
        out.push_back(static_cast<uint8_t>(c));
    }
}

inline void writeUInt64(etl::ivector<uint8_t>& out, uint32_t fieldNum, uint64_t value) {
    writeTag(out, fieldNum, ProtoWireType::Varint);
    writeVarint(out, value);
}

inline void writeBool(etl::ivector<uint8_t>& out, uint32_t fieldNum, bool value) {
    writeUInt64(out, fieldNum, value ? 1 : 0);
}

inline void writeEnum(etl::ivector<uint8_t>& out, uint32_t fieldNum, int value) {
    writeUInt64(out, fieldNum, static_cast<uint64_t>(value));
}

// Forward-only reader over a protobuf field stream. Skips fields the caller
// doesn't ask about - proto2 wire-format compatibility, and the concrete
// reason this matters here: VersionMessage has 19 fields and Tier 1 code
// only reads two of them (see QcHandshakeMessages.hpp).
class ProtoFieldReader {
public:
    struct Field {
        uint32_t fieldNum;
        ProtoWireType wireType;
        uint64_t varintValue;            // valid when wireType == Varint
        std::span<const uint8_t> bytes;  // valid for LengthDelimited/Fixed32/Fixed64
    };

    explicit ProtoFieldReader(std::span<const uint8_t> data) : data_(data) {}

    // Returns the next field, or nullopt at end of stream or on malformed
    // input (truncated varint, a length-delimited field whose length runs
    // past the end of `data`, or a reserved/group wire type this reader
    // doesn't understand).
    std::optional<Field> next() {
        if (offset_ >= data_.size()) {
            return std::nullopt;
        }

        uint64_t tag = 0;
        if (!readVarint(tag)) {
            return std::nullopt;
        }

        Field field{
            .fieldNum = static_cast<uint32_t>(tag >> 3),
            .wireType = static_cast<ProtoWireType>(tag & 0x7),
            .varintValue = 0,
            .bytes = {},
        };

        switch (field.wireType) {
        case ProtoWireType::Varint:
            if (!readVarint(field.varintValue)) {
                return std::nullopt;
            }
            break;
        case ProtoWireType::Fixed64:
            if (!takeBytes(8, field.bytes)) {
                return std::nullopt;
            }
            break;
        case ProtoWireType::LengthDelimited: {
            uint64_t len = 0;
            if (!readVarint(len) || !takeBytes(len, field.bytes)) {
                return std::nullopt;
            }
            break;
        }
        case ProtoWireType::Fixed32:
            if (!takeBytes(4, field.bytes)) {
                return std::nullopt;
            }
            break;
        default:
            return std::nullopt;
        }

        return field;
    }

private:
    bool readVarint(uint64_t& out) {
        out = 0;
        int shift = 0;
        while (offset_ < data_.size()) {
            const uint8_t byte = data_[offset_++];
            out |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) {
                return true;
            }
            shift += 7;
            if (shift >= 64) {
                return false;
            }
        }
        return false;
    }

    bool takeBytes(uint64_t len, std::span<const uint8_t>& out) {
        if (len > data_.size() - offset_) {
            return false;
        }
        out = data_.subspan(offset_, len);
        offset_ += len;
        return true;
    }

    std::span<const uint8_t> data_;
    size_t offset_ = 0;
};

}  // namespace qcbridge
