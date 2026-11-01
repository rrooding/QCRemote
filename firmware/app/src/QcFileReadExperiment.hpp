#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "QcFileMessage.hpp"
#include "QcFileRequest.hpp"
#include "QcProtobuf.hpp"

namespace qcbridge {

// issue #33 diagnostic, not part of the shipped protocol logic: before
// picking real buffer sizes, test whether FileMessage READ's
// short_listing (field 6) and omit_factory_content (field 11) options
// shrink the device's whole-catalog listing on the wire, and - if they do
// - whether the shrunken listing still names every user preset. Neither
// field number was guessed: both are verified against qc-mcp's own
// descriptor set (qc_descriptors-4.1.pb) in the issue, and qc-mcp itself
// never sets either one, so their real effect is unknown until this runs
// on hardware. See firmware/README.md's issue #33 section for the exact
// test and what each outcome means.
//
// Self-contained and meant to be deleted, along with its few call sites in
// QcSession.hpp (grep "issue #33 variant experiment"), once the owner has
// picked a variant (or decided none of them help). Plain C++23, no Zephyr
// dependency - same reasoning as QcFileStream.hpp/QcMessageTally.hpp:
// trivial to unit-test, and not thread-safe by itself - QcSession drives
// every call under its own fileStreamMutex_, the same lock it already
// holds around fileStream_/messageTally_.
class QcFileReadExperiment {
public:
    struct Variant {
        std::string_view label;
        FileReadOptions options;
    };

    static constexpr std::array<Variant, 4> kVariants = {{
        {"default", FileReadOptions{}},
        {"short_listing=true", FileReadOptions{.shortListing = true}},
        {"omit_factory_content=true", FileReadOptions{.omitFactoryContent = true}},
        {"short_listing=true omit_factory_content=true",
         FileReadOptions{.shortListing = true, .omitFactoryContent = true}},
    }};

    // 4 covers every type value actually expected (-1 missing, 0 presets, 1
    // IRs, 2 captures) with one slot of headroom - kept small deliberately:
    // this struct gets copied out of QcSession's locked section and handed
    // to runHeartbeat() across a stack frame boundary (qc_heartbeat's
    // 2048-byte stack has little margin - see QcSession.hpp's
    // logFileStreamSummaryAndAdvance()).
    static constexpr size_t kMaxTypeEntries = 4;
    // Low-rate sampling (onFileMessage()'s onSample callback): every
    // kSampleInterval-th preset seen, up to kMaxSamples total, per variant -
    // enough to see whether names survive the option(s) without flooding
    // the log the way one line per preset (hundreds, across four variants)
    // would.
    static constexpr uint32_t kSampleInterval = 47;  // prime, to avoid aliasing with folder sizes
    static constexpr uint32_t kMaxSamples = 8;

    struct TypeCount {
        int64_t type = 0;
        uint32_t count = 0;
    };

    struct ContentSummary {
        uint32_t presetFolderCount = 0;
        uint32_t totalPresetCount = 0;
        uint32_t emptyNamePresetCount = 0;
        std::array<TypeCount, kMaxTypeEntries> typeCounts{};
        size_t typeCountEntries = 0;
        uint32_t otherTypeCount = 0;
    };

    // Call once per connect, before sending variant 0's File READ - also
    // safe to call again on a reconnect so a second connect starts back at
    // variant 0 rather than resuming mid-sequence.
    void reset() { variantIndex_ = 0; }

    bool hasCurrentVariant() const { return variantIndex_ < kVariants.size(); }
    const Variant& currentVariant() const { return kVariants[variantIndex_]; }
    bool isLastVariant() const { return variantIndex_ + 1 >= kVariants.size(); }
    void advanceVariant() { ++variantIndex_; }
    // 1-based, for "sending variant N/4" logging - index+1, not clamped.
    size_t variantNumber() const { return variantIndex_ + 1; }

    // Resets the per-variant content counters - call once right before
    // sending that variant's File READ, mirrors QcFileStream::start()'s
    // "fresh window" contract.
    void beginVariant() {
        presetFolderCount_ = 0;
        totalPresetCount_ = 0;
        emptyNamePresetCount_ = 0;
        presetsSeen_ = 0;
        samplesLogged_ = 0;
        typeCounts_ = {};
        typeCountEntries_ = 0;
        otherTypeCount_ = 0;
    }

    // Called from onMessage() (dispatcher_/gzipInflater_'s own worker
    // thread, never usbh) for every File message that reached it during
    // the current variant - i.e. one that fit kReassemblyCapacity and (if
    // compressed) inflated within kMaxInflatedSize. Decodes the message's
    // folder type and presets right here using QcFileMessage.hpp's
    // decodeFile() (ADR 0006), the same place that decoder was always
    // meant to run from: no recursion, no heap allocation, and no local
    // bigger than a handful of pointers/ints/bools, so this adds only a
    // small, fixed number of stack frames to whichever worker thread calls
    // it (qc_dispatch's 2048 bytes or qc_gzip_inflate's 4096) - nothing
    // like the stack cost HidReassembler::appendTail()'s unbounded scratch
    // buffer (this issue's own earlier hardware MPU fault) or a bare
    // LOG_INF on usbh (issue #8) once cost this firmware.
    //
    // `onSample` is invoked for a bounded, low-rate subset of presets (see
    // kSampleInterval/kMaxSamples) with (folderName, presetName), letting
    // the caller log a few name pairs without this class depending on
    // Zephyr's logging headers at all (keeps it host/qemu ztest-friendly,
    // same reasoning as QcFileStream.hpp).
    template <typename SampleCallback>
    void onFileMessage(std::span<const uint8_t> protoBytes, SampleCallback&& onSample) {
        const auto type = peekFileType(protoBytes);
        recordType(type);
        if (type == kFileTypePresets) {
            ++presetFolderCount_;
        }
        decodeFile(protoBytes, [&](const FilePreset& preset) {
            ++totalPresetCount_;
            if (preset.presetName.empty()) {
                ++emptyNamePresetCount_;
            }
            const uint32_t seen = ++presetsSeen_;
            if (samplesLogged_ < kMaxSamples && (seen % kSampleInterval) == 1) {
                onSample(preset.folderName, preset.presetName);
                ++samplesLogged_;
            }
        });
    }

    ContentSummary contentSummary() const {
        ContentSummary summary;
        summary.presetFolderCount = presetFolderCount_;
        summary.totalPresetCount = totalPresetCount_;
        summary.emptyNamePresetCount = emptyNamePresetCount_;
        summary.typeCounts = typeCounts_;
        summary.typeCountEntries = typeCountEntries_;
        summary.otherTypeCount = otherTypeCount_;
        return summary;
    }

private:
    static std::optional<int64_t> peekFileType(std::span<const uint8_t> protoBytes) {
        ProtoFieldReader reader(protoBytes);
        while (const auto field = reader.next()) {
            if (field->fieldNum == 3 && field->wireType == ProtoWireType::Varint) {
                return static_cast<int64_t>(field->varintValue);
            }
        }
        return std::nullopt;
    }

    void recordType(std::optional<int64_t> type) {
        const int64_t t = type.value_or(-1);  // -1: no `type` field present on this message
        for (size_t i = 0; i < typeCountEntries_; ++i) {
            if (typeCounts_[i].type == t) {
                ++typeCounts_[i].count;
                return;
            }
        }
        if (typeCountEntries_ < typeCounts_.size()) {
            typeCounts_[typeCountEntries_++] = TypeCount{.type = t, .count = 1};
            return;
        }
        ++otherTypeCount_;
    }

    size_t variantIndex_ = 0;

    uint32_t presetFolderCount_ = 0;
    uint32_t totalPresetCount_ = 0;
    uint32_t emptyNamePresetCount_ = 0;
    uint32_t presetsSeen_ = 0;
    uint32_t samplesLogged_ = 0;
    std::array<TypeCount, kMaxTypeEntries> typeCounts_{};
    size_t typeCountEntries_ = 0;
    uint32_t otherTypeCount_ = 0;
};

}  // namespace qcbridge
