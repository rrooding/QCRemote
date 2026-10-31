#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "QcProtobuf.hpp"

namespace qcbridge {

// CortexMessageType.Enum command id for File (protocol.py's COMMANDS map;
// issue #9's directory read).
inline constexpr uint16_t kCommandFile = 4;

// FileMessage.type values (qc-mcp's directory.py FILE_TYPE). Only Presets is
// in scope here - IRs/Captures folders exist in the same stream but aren't
// decoded by this message, matching structure_directory()'s own filtering.
inline constexpr int64_t kFileTypePresets = 0;

// One preset entry, decoded in place from the caller's already-inflated
// FileMessage bytes - every field below is a view into that buffer, not a
// copy (see docs/adr/0006-file-message-codec.md).
struct FilePreset {
    std::string_view folderKey;
    std::string_view folderName;
    bool folderIsFactory = false;
    std::string_view presetName;
    uint32_t index = 0;
};

namespace detail {
inline std::string_view toStringView(std::span<const uint8_t> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
}  // namespace detail

// Decodes a FileMessage's protobuf bytes (the trailer already stripped by
// decodeMessage()) and invokes `onPreset` once per preset found in its
// folder, in stream order. Non-preset folders (type != 0) and messages with
// no folder at all are not malformed - they just produce zero callbacks.
//
// `index` is the preset's position within the folder's `files` array, not
// ProductData's own `index` field - on a directory READ the device leaves
// that field at 0 and the array position is authoritative (qc-mcp's
// directory.py `_file_dict()`).
//
// Returns false only for empty input; any other malformed/truncated byte
// sequence is handled the same way ADR 0003's reader always has - decoding
// stops at the first bad field and whatever was already parsed stands,
// without ever reading past the slice it was given.
template <typename Callback>
inline bool decodeFile(std::span<const uint8_t> protoBytes, Callback&& onPreset) {
    if (protoBytes.empty()) {
        return false;
    }

    std::span<const uint8_t> folderBytes;
    bool hasFolder = false;
    int64_t type = -1;

    ProtoFieldReader reader(protoBytes);
    while (const auto field = reader.next()) {
        switch (field->fieldNum) {
        case 3:
            if (field->wireType == ProtoWireType::Varint) {
                type = static_cast<int64_t>(field->varintValue);
            }
            break;
        case 4:
            if (field->wireType == ProtoWireType::LengthDelimited) {
                folderBytes = field->bytes;
                hasFolder = true;
            }
            break;
        default:
            break;
        }
    }

    if (!hasFolder || type != kFileTypePresets) {
        return true;
    }

    // Pass 1: folder metadata (key/name/is_factory), order-independent of
    // the repeated `files` field below.
    std::string_view folderKey;
    std::string_view folderName;
    bool folderIsFactory = false;
    {
        ProtoFieldReader folderReader(folderBytes);
        while (const auto field = folderReader.next()) {
            switch (field->fieldNum) {
            case 1:
                if (field->wireType == ProtoWireType::LengthDelimited) {
                    folderKey = detail::toStringView(field->bytes);
                }
                break;
            case 3:
                if (field->wireType == ProtoWireType::LengthDelimited) {
                    folderName = detail::toStringView(field->bytes);
                }
                break;
            case 4:
                if (field->wireType == ProtoWireType::Varint) {
                    folderIsFactory = field->varintValue != 0;
                }
                break;
            default:
                break;
            }
        }
    }

    // Pass 2: each repeated `files` entry (field 7, one ProductData per
    // occurrence), now that folder metadata is known regardless of where
    // `files` fell in the wire stream.
    uint32_t index = 0;
    ProtoFieldReader filesReader(folderBytes);
    while (const auto field = filesReader.next()) {
        if (field->fieldNum != 7 || field->wireType != ProtoWireType::LengthDelimited) {
            continue;
        }

        std::string_view presetName;
        ProtoFieldReader productReader(field->bytes);
        while (const auto productField = productReader.next()) {
            if (productField->fieldNum == 3 && productField->wireType == ProtoWireType::LengthDelimited) {
                presetName = detail::toStringView(productField->bytes);
            }
        }

        onPreset(FilePreset{folderKey, folderName, folderIsFactory, presetName, index});
        ++index;
    }

    return true;
}

}  // namespace qcbridge
