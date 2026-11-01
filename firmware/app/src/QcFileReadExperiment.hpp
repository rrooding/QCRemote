#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <etl/string.h>

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
// dependency (etl is header-only and heap-free, same as every other
// bounded-string use in this codebase - see CODING_STANDARDS.md) - same
// reasoning as QcFileStream.hpp/QcMessageTally.hpp: trivial to unit-test,
// and not thread-safe by itself - QcSession drives every call under its
// own fileStreamMutex_, the same lock it already holds around
// fileStream_/messageTally_.
class QcFileReadExperiment {
public:
    struct Variant {
        std::string_view label;
        FileReadOptions options;
    };

    // Order matters here: a first hardware run (commit 147fb48) sending
    // default first found its reply still trickling in when the next
    // variant's request went out, contaminating that next variant's count
    // (QcSession's inter-variant drain gap - see runHeartbeat() - also
    // guards against this, but ordering the cheapest/most-likely-useful
    // options first means a contaminated tail lands on `default`, whose
    // real numbers are already known from `a1c25b6`'s measurement, rather
    // than on an option this experiment is actually trying to measure.
    // `default` last also then directly re-confirms `a1c25b6` under this
    // run's conditions.
    static constexpr std::array<Variant, 4> kVariants = {{
        {"short_listing=true", FileReadOptions{.shortListing = true}},
        {"omit_factory_content=true", FileReadOptions{.omitFactoryContent = true}},
        {"short_listing=true omit_factory_content=true",
         FileReadOptions{.shortListing = true, .omitFactoryContent = true}},
        {"default", FileReadOptions{}},
    }};

    // 4 covers every type value actually expected (-1 missing/ack, 0
    // presets, 1 IRs, 2 captures) with one slot of headroom.
    static constexpr size_t kMaxTypeEntries = 4;

    // Per-folder detail (name/key/is_factory/is_downloads/parent_key/
    // named+empty counts/samples), for the two `short_listing=true`
    // variants only (QcSession::logFileStreamSummaryAndAdvance() decides
    // whether to print it) - the first hardware run found `short_listing`
    // shrinks the catalog to ~13 folders, so this is sized for that, not
    // for `default`/`omit_factory_content` alone's few hundred. A folder
    // beyond this capacity (expected only for the non-short-listing
    // variants, where this table isn't printed) falls into
    // otherFolderCount_ rather than growing unboundedly.
    static constexpr size_t kMaxFolders = 16;
    static constexpr size_t kMaxSamplesPerFolder = 3;
    static constexpr size_t kFolderStringCapacity = 64;
    static constexpr size_t kSampleNameCapacity = 48;

    struct TypeCount {
        int64_t type = 0;
        uint32_t count = 0;
    };

    struct FolderDetail {
        etl::string<kFolderStringCapacity> key;
        etl::string<kFolderStringCapacity> name;
        etl::string<kFolderStringCapacity> parentKey;
        bool isFactory = false;
        bool isDownloads = false;
        uint32_t namedCount = 0;
        uint32_t emptyCount = 0;
        std::array<etl::string<kSampleNameCapacity>, kMaxSamplesPerFolder> samples{};
        size_t sampleCount = 0;
    };

    struct ContentSummary {
        uint32_t presetFolderCount = 0;
        uint32_t factoryFolderCount = 0;
        uint32_t userFolderCount = 0;
        // "Named" below excludes empty-slot entries - the device's
        // SetlistPosition table is documented (qc-mcp's directory.py) to
        // include empty slots with a blank name, and hardware evidence
        // (issue #33) shows File folders do the same, so an empty name is
        // an unused slot, not a preset, and shouldn't be reported as one.
        uint32_t totalPresetCount = 0;  // named + empty-slot entries
        uint32_t emptyNamePresetCount = 0;
        uint32_t namedFactoryPresetCount = 0;
        uint32_t namedUserPresetCount = 0;
        std::array<TypeCount, kMaxTypeEntries> typeCounts{};
        size_t typeCountEntries = 0;
        uint32_t otherTypeCount = 0;
        uint32_t otherFolderCount = 0;
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
    //
    // Deliberately does NOT reset `folders_` itself (a real hardware
    // crash, confirmed via addr2line against this exact line: a stack
    // overflow on qc_session's 2048-byte stack, right after the
    // handshake, before the first variant's File READ even went out).
    // `folders_ = {}` assigns a freshly-constructed std::array<FolderDetail,
    // kMaxFolders> - 16 non-trivial objects, each holding several
    // etl::string members - and at this build's optimization level that
    // materializes the whole ~7KB temporary on the calling thread's stack
    // before the assignment, the same class of bug as the two earlier
    // stack overflows this issue has already hit (HidReassembler::
    // appendTail(), then the inline per-variant logging in
    // runHeartbeat()). folderCount_ = 0 below is enough on its own:
    // folderDetails() only ever returns {folders_.data(), folderCount_},
    // and recordFolder()'s own key search is bounded by folderCount_ too,
    // so stale bytes past it are never read by anything - and fillFolder()
    // (called for every slot recordFolder() claims, new or reused) resets
    // every field that selectively feeds a sum, including the counts and
    // sample count, so a slot index reused from a previous variant can't
    // leak that variant's counts into this one's.
    void beginVariant() {
        presetFolderCount_ = 0;
        factoryFolderCount_ = 0;
        userFolderCount_ = 0;
        totalPresetCount_ = 0;
        emptyNamePresetCount_ = 0;
        namedFactoryPresetCount_ = 0;
        namedUserPresetCount_ = 0;
        typeCounts_ = {};
        typeCountEntries_ = 0;
        otherTypeCount_ = 0;
        folderCount_ = 0;
        otherFolderCount_ = 0;
    }

    // A File message with no `folder` field at all (QcSession checks this
    // via hasFolder() before deciding whether to feed the message to
    // QcFileStream's quiet-window tracking at all - see that call site's
    // comment). A real catalog entry always carries a folder (every
    // message this experiment has seen for `default`/`omit_factory_content`
    // does); a message without one is something else - on the first
    // hardware run, a single 14-byte reply with no `type` either (shows up
    // here as a type=-1 entry in the type tally) arrived alone for
    // `short_listing=true` and was mistaken for the whole reply. What it
    // actually is isn't confirmed - qc-mcp never sets `short_listing`, so
    // it has no code path that would explain this reply shape - but
    // whatever it is, it isn't the listing itself, so it must not be
    // allowed to end the stream on its own.
    static bool hasFolder(std::span<const uint8_t> protoBytes) { return peekFolderMeta(protoBytes).has_value(); }

    // Called from onMessage() (dispatcher_/gzipInflater_'s own worker
    // thread, never usbh) for every File message that reached it during
    // the current variant - i.e. one that fit kReassemblyCapacity and (if
    // compressed) inflated within kMaxInflatedSize. Decodes the message's
    // folder/type and presets right here using QcFileMessage.hpp's
    // decodeFile() (ADR 0006), the same place that decoder was always
    // meant to run from: no recursion, no heap allocation, and no local
    // bigger than a small fixed-capacity string, so this adds only a
    // small, fixed number of stack frames to whichever worker thread calls
    // it (qc_dispatch's 2048 bytes or qc_gzip_inflate's 4096) - nothing
    // like the stack cost HidReassembler::appendTail()'s unbounded scratch
    // buffer (this issue's own earlier hardware MPU fault) or a bare
    // LOG_INF on usbh (issue #8) once cost this firmware.
    //
    // Deliberately does no logging of its own (a real hardware run found
    // `dispatcherDropped` turn nonzero for the first time the moment this
    // class started logging a sample inline here - see
    // firmware/README.md's issue #33 section: CONFIG_LOG_MODE_IMMEDIATE
    // blocks the calling thread for a real UART write, during which the
    // single-slot dispatcher mailbox can't accept the device's next
    // message). Everything decoded here is only *accumulated*; printing
    // it happens later, off this thread entirely, from
    // QcSession::logFileStreamSummaryAndAdvance() on the heartbeat thread,
    // once the whole variant's stream has gone quiet.
    void onFileMessage(std::span<const uint8_t> protoBytes) {
        const auto type = peekFileType(protoBytes);
        recordType(type);

        const auto folderMeta = peekFolderMeta(protoBytes);
        if (type != kFileTypePresets || !folderMeta) {
            return;
        }

        if (folderMeta->isFactory) {
            ++factoryFolderCount_;
        } else {
            ++userFolderCount_;
        }
        ++presetFolderCount_;
        FolderDetail* detail = recordFolder(*folderMeta);

        decodeFile(protoBytes, [&](const FilePreset& preset) {
            ++totalPresetCount_;
            const bool named = !preset.presetName.empty();
            if (!named) {
                ++emptyNamePresetCount_;
            } else if (preset.folderIsFactory) {
                ++namedFactoryPresetCount_;
            } else {
                ++namedUserPresetCount_;
            }
            if (detail == nullptr) {
                return;
            }
            if (named) {
                ++detail->namedCount;
                if (detail->sampleCount < detail->samples.size()) {
                    detail->samples[detail->sampleCount].assign(preset.presetName.data(),
                                                                 preset.presetName.size());
                    ++detail->sampleCount;
                }
            } else {
                ++detail->emptyCount;
            }
        });
    }

    ContentSummary contentSummary() const {
        return ContentSummary{
            .presetFolderCount = presetFolderCount_,
            .factoryFolderCount = factoryFolderCount_,
            .userFolderCount = userFolderCount_,
            .totalPresetCount = totalPresetCount_,
            .emptyNamePresetCount = emptyNamePresetCount_,
            .namedFactoryPresetCount = namedFactoryPresetCount_,
            .namedUserPresetCount = namedUserPresetCount_,
            .typeCounts = typeCounts_,
            .typeCountEntries = typeCountEntries_,
            .otherTypeCount = otherTypeCount_,
            .otherFolderCount = otherFolderCount_,
        };
    }

    // Not a copy - a view into this object's own storage, valid as long as
    // this QcFileReadExperiment is (see QcSession::logFileStreamSummaryAndAdvance()
    // for why reading this without a lock is safe there).
    std::span<const FolderDetail> folderDetails() const { return {folders_.data(), folderCount_}; }

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

    struct FolderMeta {
        std::string_view key;
        std::string_view name;
        std::string_view parentKey;
        bool isFactory = false;
        bool isDownloads = false;
    };

    // FolderInfo field numbers (1=key, 2=parent_key, 3=name, 4=is_factory,
    // 5=is_downloads) verified against qc-mcp's descriptor set, same as
    // every other field number this issue has used - QcFileMessage.hpp's
    // decodeFile() only exposes key/name/is_factory (via FilePreset), not
    // parent_key/is_downloads, so this duplicates a small amount of its
    // parsing rather than changing that already-tested decoder's shape for
    // a diagnostic-only need.
    static std::optional<FolderMeta> peekFolderMeta(std::span<const uint8_t> protoBytes) {
        ProtoFieldReader reader(protoBytes);
        std::span<const uint8_t> folderBytes;
        bool hasFolderField = false;
        while (const auto field = reader.next()) {
            if (field->fieldNum == 4 && field->wireType == ProtoWireType::LengthDelimited) {
                folderBytes = field->bytes;
                hasFolderField = true;
            }
        }
        if (!hasFolderField) {
            return std::nullopt;
        }

        FolderMeta meta;
        ProtoFieldReader folderReader(folderBytes);
        while (const auto field = folderReader.next()) {
            switch (field->fieldNum) {
            case 1:
                if (field->wireType == ProtoWireType::LengthDelimited) {
                    meta.key = detail::toStringView(field->bytes);
                }
                break;
            case 2:
                if (field->wireType == ProtoWireType::LengthDelimited) {
                    meta.parentKey = detail::toStringView(field->bytes);
                }
                break;
            case 3:
                if (field->wireType == ProtoWireType::LengthDelimited) {
                    meta.name = detail::toStringView(field->bytes);
                }
                break;
            case 4:
                if (field->wireType == ProtoWireType::Varint) {
                    meta.isFactory = field->varintValue != 0;
                }
                break;
            case 5:
                if (field->wireType == ProtoWireType::Varint) {
                    meta.isDownloads = field->varintValue != 0;
                }
                break;
            default:
                break;
            }
        }
        return meta;
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

    // Finds this folder key's existing slot, or claims a new one (up to
    // kMaxFolders). Either way, fillFolder() below resets that slot's
    // counts/samples along with its metadata - qc-mcp's own directory.py
    // comment is explicit that "later messages for the same folder key
    // replace earlier ones," not merge with them, so a resent folder's
    // content shouldn't accumulate across messages, and a brand-new slot
    // reused from a previous *variant* (beginVariant() only resets
    // folderCount_, not folders_ itself - see that function's comment)
    // can't leak that variant's stale counts into this one's either.
    // Returns nullptr once kMaxFolders is exhausted (bumps
    // otherFolderCount_ instead) - expected only for the non-short-listing
    // variants, whose few-hundred folders this table was never sized for
    // and isn't printed for anyway (see kMaxFolders' own comment).
    FolderDetail* recordFolder(const FolderMeta& meta) {
        for (size_t i = 0; i < folderCount_; ++i) {
            if (std::string_view(folders_[i].key.c_str(), folders_[i].key.size()) == meta.key) {
                fillFolder(folders_[i], meta);
                return &folders_[i];
            }
        }
        if (folderCount_ < folders_.size()) {
            FolderDetail& slot = folders_[folderCount_++];
            fillFolder(slot, meta);
            return &slot;
        }
        ++otherFolderCount_;
        return nullptr;
    }

    // Resets every field of `slot` - not just the metadata assign() fills
    // in, but also the counts/sample bookkeeping onFileMessage() goes on
    // to update - so every caller (a resent key, or a slot index reused
    // from an earlier variant) starts this folder completely fresh.
    // Deliberately field-by-field, not `slot = FolderDetail{}`: the latter
    // constructs a whole fresh FolderDetail as a temporary first, which is
    // exactly the pattern that overflowed qc_session's stack when done
    // for all 16 slots at once in beginVariant() (see that function's
    // comment) - one slot's worth is small enough not to matter here, but
    // there's no reason to take the chance twice in the same class.
    static void fillFolder(FolderDetail& slot, const FolderMeta& meta) {
        slot.key.assign(meta.key.data(), meta.key.size());
        slot.name.assign(meta.name.data(), meta.name.size());
        slot.parentKey.assign(meta.parentKey.data(), meta.parentKey.size());
        slot.isFactory = meta.isFactory;
        slot.isDownloads = meta.isDownloads;
        slot.namedCount = 0;
        slot.emptyCount = 0;
        slot.sampleCount = 0;
    }

    size_t variantIndex_ = 0;

    uint32_t presetFolderCount_ = 0;
    uint32_t factoryFolderCount_ = 0;
    uint32_t userFolderCount_ = 0;
    uint32_t totalPresetCount_ = 0;
    uint32_t emptyNamePresetCount_ = 0;
    uint32_t namedFactoryPresetCount_ = 0;
    uint32_t namedUserPresetCount_ = 0;
    std::array<TypeCount, kMaxTypeEntries> typeCounts_{};
    size_t typeCountEntries_ = 0;
    uint32_t otherTypeCount_ = 0;

    std::array<FolderDetail, kMaxFolders> folders_{};
    size_t folderCount_ = 0;
    uint32_t otherFolderCount_ = 0;
};

}  // namespace qcbridge
