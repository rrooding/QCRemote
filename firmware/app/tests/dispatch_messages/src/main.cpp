#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "QcFileReadExperiment.hpp"
#include "QcFileRequest.hpp"
#include "QcFileStream.hpp"
#include "QcMessageTally.hpp"
#include "QcReassemblyOverflowTally.hpp"
#include "QcStateReads.hpp"

using namespace qcbridge;

ZTEST_SUITE(dispatch_messages, NULL, NULL, NULL, NULL, NULL);

// --- QcFileRequest.hpp ---------------------------------------------------

ZTEST(dispatch_messages, test_encode_file_read)
{
    HandshakeMessageBuffer out;
    encodeFileRead(out, 7);

    const std::vector<uint8_t> expected = {
        0x08, 0x03,              // action = READ (3)
        0x10, 0x07,              // request_id = 7
        0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=4 (File)
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(dispatch_messages, test_encode_file_read_short_listing)
{
    // issue #33 variant experiment: field 6, verified against qc-mcp's own
    // descriptor set (qc_descriptors-4.1.pb).
    HandshakeMessageBuffer out;
    encodeFileRead(out, 7, FileReadOptions{.shortListing = true});

    const std::vector<uint8_t> expected = {
        0x08, 0x03,              // action = READ (3)
        0x10, 0x07,              // request_id = 7
        0x30, 0x01,              // short_listing = true (field 6)
        0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=4 (File)
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(dispatch_messages, test_encode_file_read_omit_factory_content)
{
    // issue #33 variant experiment: field 11, verified against qc-mcp's own
    // descriptor set (qc_descriptors-4.1.pb).
    HandshakeMessageBuffer out;
    encodeFileRead(out, 7, FileReadOptions{.omitFactoryContent = true});

    const std::vector<uint8_t> expected = {
        0x08, 0x03,              // action = READ (3)
        0x10, 0x07,              // request_id = 7
        0x58, 0x01,              // omit_factory_content = true (field 11)
        0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=4 (File)
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(dispatch_messages, test_encode_file_read_both_options)
{
    HandshakeMessageBuffer out;
    encodeFileRead(out, 7, FileReadOptions{.shortListing = true, .omitFactoryContent = true});

    const std::vector<uint8_t> expected = {
        0x08, 0x03,              // action = READ (3)
        0x10, 0x07,              // request_id = 7
        0x30, 0x01,              // short_listing = true (field 6)
        0x58, 0x01,              // omit_factory_content = true (field 11)
        0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=4 (File)
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

// --- QcFileStream.hpp ------------------------------------------------------

ZTEST(dispatch_messages, test_file_stream_starts_inactive_until_started)
{
    QcFileStream stream;
    zassert_false(stream.isActive(), "expected inactive before start()");

    stream.start(1000);
    zassert_true(stream.isActive(), "expected active after start()");
}

ZTEST(dispatch_messages, test_file_stream_accumulates_messages)
{
    QcFileStream stream;
    stream.start(0);

    stream.onMessage(10, 100, 100, false);
    stream.onMessage(20, 50, 200, true);
    stream.onInflateFailed(30, 75);

    // Not quiet yet (last activity at t=30, window is 1500ms) and nowhere
    // near the 30s overall cap.
    zassert_false(stream.checkTimeout(40).has_value(), "expected still collecting");
}

ZTEST(dispatch_messages, test_file_stream_finishes_on_quiet_window)
{
    QcFileStream stream;
    stream.start(0);

    stream.onMessage(10, 100, 100, false);   // wire=100 inflated=100
    stream.onMessage(500, 50, 200, true);    // wire=50 inflated=200, compressed
    stream.onInflateFailed(900, 75);         // wire=75, failed

    // Last activity at t=900; one ms short of the 1500ms quiet window.
    zassert_false(stream.checkTimeout(900 + QcFileStream::kQuietWindowMs - 1).has_value(),
                  "expected still collecting just under the quiet window");

    const auto summary = stream.checkTimeout(900 + QcFileStream::kQuietWindowMs);
    zassert_true(summary.has_value(), "expected the stream to finish on the quiet window");
    zassert_equal(summary->messageCount, 3u, "messageCount");
    zassert_equal(summary->compressedCount, 1u, "compressedCount");
    zassert_equal(summary->failedCount, 1u, "failedCount");
    zassert_equal(summary->totalWireBytes, 100u + 50u + 75u, "totalWireBytes");
    zassert_equal(summary->totalInflatedBytes, 100u + 200u, "totalInflatedBytes");
    zassert_equal(summary->maxWireBytes, 100u, "maxWireBytes");       // max(100, 50, 75)
    zassert_equal(summary->maxInflatedBytes, 200u, "maxInflatedBytes");  // max(100, 200)
    zassert_false(summary->timedOut, "expected a quiet finish, not a timeout");

    // Not computed by QcFileStream itself - see Summary's own comment.
    // Left at the struct's defaults here; QcSession is what fills these in.
    zassert_equal(summary->dispatcherDropped, 0u, "dispatcherDropped default");
    zassert_equal(summary->gzipDropped, 0u, "gzipDropped default");
    zassert_equal(summary->reassemblyOverflowCount, 0u, "reassemblyOverflowCount default");
    zassert_equal(summary->reassemblyMalformedCount, 0u, "reassemblyMalformedCount default");

    zassert_false(stream.isActive(), "expected the stream to deactivate once finished");
}

ZTEST(dispatch_messages, test_file_stream_tracks_max_not_last)
{
    QcFileStream stream;
    stream.start(0);

    // Biggest message isn't the last one - maxWireBytes/maxInflatedBytes
    // must track the running max, not just the most recent value.
    stream.onMessage(10, 50, 60, false);
    stream.onMessage(20, 4096, 16384, true);  // the peak
    stream.onMessage(30, 10, 10, false);
    stream.onInflateFailed(40, 2000);  // bigger wire size, but no inflated size to report

    const auto summary = stream.checkTimeout(40 + QcFileStream::kQuietWindowMs);
    zassert_true(summary.has_value(), "expected the stream to finish");
    zassert_equal(summary->maxWireBytes, 4096u, "maxWireBytes should track the peak, not the last value");
    zassert_equal(summary->maxInflatedBytes, 16384u, "maxInflatedBytes should track the peak");
}

ZTEST(dispatch_messages, test_file_stream_inflate_failed_true_size_feeds_max_inflated_bytes)
{
    // issue #33: a message that failed to inflate because it was too big
    // for the output buffer still has a real, known decompressed size
    // (QcGzip.hpp's peekDecompressedSize(), read off the gzip trailer) -
    // that must still count toward maxInflatedBytes, otherwise the summary
    // understates the true peak for exactly the case that matters most for
    // sizing a bigger buffer.
    QcFileStream stream;
    stream.start(0);

    stream.onMessage(10, 100, 100, false);
    stream.onInflateFailed(20, 2408, 20480);  // too big for kMaxInflatedSize, true size known

    const auto summary = stream.checkTimeout(20 + QcFileStream::kQuietWindowMs);
    zassert_true(summary.has_value(), "expected the stream to finish");
    zassert_equal(summary->maxInflatedBytes, 20480u,
                  "maxInflatedBytes should reflect the failed message's true size");
    zassert_equal(summary->totalInflatedBytes, 100u,
                  "totalInflatedBytes should NOT credit a failed inflate's bytes (none were produced)");
}

ZTEST(dispatch_messages, test_file_stream_inflate_failed_without_true_size_leaves_max_inflated_bytes_alone)
{
    QcFileStream stream;
    stream.start(0);

    stream.onMessage(10, 50, 9000, false);
    stream.onInflateFailed(20, 75);  // no third argument - true size unknown (e.g. truncated input)

    const auto summary = stream.checkTimeout(20 + QcFileStream::kQuietWindowMs);
    zassert_true(summary.has_value(), "expected the stream to finish");
    zassert_equal(summary->maxInflatedBytes, 9000u,
                  "maxInflatedBytes should be unaffected by an unknown true size");
}

ZTEST(dispatch_messages, test_file_stream_finishes_once_only)
{
    QcFileStream stream;
    stream.start(0);
    stream.onMessage(10, 10, 10, false);

    zassert_true(stream.checkTimeout(10 + QcFileStream::kQuietWindowMs).has_value(),
                 "expected the first check past the quiet window to finish");
    zassert_false(stream.checkTimeout(10 + QcFileStream::kQuietWindowMs + 1000).has_value(),
                  "expected no second summary for an already-finished stream");

    // A message arriving after the stream already finished is dropped, not
    // folded into a stale summary.
    stream.onMessage(20000, 10, 10, false);
    zassert_false(stream.isActive(), "expected a late message not to reactivate the stream");
}

ZTEST(dispatch_messages, test_file_stream_overall_timeout_fires_even_with_no_messages)
{
    QcFileStream stream;
    stream.start(0);

    // No messages at all - the quiet-window condition can never fire
    // (messageCount_ == 0), but the 30s overall cap still must.
    zassert_false(stream.checkTimeout(QcFileStream::kOverallTimeoutMs - 1).has_value(),
                  "expected still collecting just under the overall timeout");

    const auto summary = stream.checkTimeout(QcFileStream::kOverallTimeoutMs);
    zassert_true(summary.has_value(), "expected the overall timeout to finish the stream");
    zassert_equal(summary->messageCount, 0u, "messageCount");
    zassert_true(summary->timedOut, "expected a timeout finish");
}

ZTEST(dispatch_messages, test_file_stream_restarts_cleanly)
{
    QcFileStream stream;
    stream.start(0);
    stream.onMessage(10, 999, 999, true);
    const auto first = stream.checkTimeout(10 + QcFileStream::kQuietWindowMs);
    zassert_true(first.has_value(), "expected the first run to finish");
    zassert_equal(first->messageCount, 1u, "first run messageCount");

    // A fresh start() must not carry over the previous run's counts.
    stream.start(100000);
    zassert_true(stream.isActive(), "expected active after restart");
    const auto tooSoon = stream.checkTimeout(100000 + QcFileStream::kQuietWindowMs - 1);
    zassert_false(tooSoon.has_value(), "expected the new run to still be collecting");

    stream.onMessage(100100, 1, 1, false);
    const auto second = stream.checkTimeout(100100 + QcFileStream::kQuietWindowMs);
    zassert_true(second.has_value(), "expected the second run to finish");
    zassert_equal(second->messageCount, 1u, "second run messageCount should not include the first run's");
}

ZTEST(dispatch_messages, test_file_stream_quiet_window_is_overridable)
{
    // issue #33 variant experiment: a longer-than-default quiet window,
    // passed to start(), so a mid-reply pause doesn't look like the reply
    // ending (firmware/README.md's issue #33 section).
    QcFileStream stream;
    const int64_t longWindow = QcFileStream::kQuietWindowMs * 3;
    stream.start(0, longWindow);
    stream.onMessage(10, 10, 10, false);

    zassert_false(stream.checkTimeout(10 + QcFileStream::kQuietWindowMs).has_value(),
                 "the default window alone must not finish an overridden stream");
    const auto summary = stream.checkTimeout(10 + longWindow);
    zassert_true(summary.has_value(), "expected the overridden window to finish the stream");
}

ZTEST(dispatch_messages, test_file_stream_quiet_window_defaults_when_not_overridden)
{
    // Existing single-argument start() calls (every other test in this
    // suite) must keep compiling and behaving the same way.
    QcFileStream stream;
    stream.start(0);
    stream.onMessage(10, 10, 10, false);
    zassert_true(stream.checkTimeout(10 + QcFileStream::kQuietWindowMs).has_value(),
                "expected the default quiet window to still apply");
}

// --- QcMessageTally.hpp ----------------------------------------------------

ZTEST(dispatch_messages, test_message_tally_ignores_record_before_start)
{
    QcMessageTally tally;
    zassert_false(tally.isActive(), "expected inactive before start()");

    tally.record(4);
    zassert_equal(tally.entries().size(), 0u, "a record before start() must be dropped");
    zassert_equal(tally.otherCount(), 0u, "otherCount must stay 0 too");
}

ZTEST(dispatch_messages, test_message_tally_counts_per_command)
{
    QcMessageTally tally;
    tally.start();

    tally.record(4);
    tally.record(1);
    tally.record(4);
    tally.record(4);
    tally.record(1);

    const auto entries = tally.entries();
    zassert_equal(entries.size(), 2u, "expected two distinct commands");
    zassert_equal(entries[0].command, 4u, "first distinct command seen, in order");
    zassert_equal(entries[0].count, 3u, "command 4 count");
    zassert_equal(entries[1].command, 1u, "second distinct command seen, in order");
    zassert_equal(entries[1].count, 2u, "command 1 count");
    zassert_equal(tally.otherCount(), 0u, "otherCount should stay 0 within capacity");
}

ZTEST(dispatch_messages, test_message_tally_overflow_falls_into_other)
{
    QcMessageTally tally;
    tally.start();

    for (uint16_t command = 0; command < QcMessageTally::kMaxDistinctCommands; ++command) {
        tally.record(command);
    }
    // One more call per already-tracked command plus three brand-new ones -
    // the former must still find their existing entry, the latter must
    // overflow into otherCount() rather than growing the array.
    tally.record(0);
    tally.record(static_cast<uint16_t>(QcMessageTally::kMaxDistinctCommands));
    tally.record(static_cast<uint16_t>(QcMessageTally::kMaxDistinctCommands + 1));
    tally.record(static_cast<uint16_t>(QcMessageTally::kMaxDistinctCommands + 2));

    zassert_equal(tally.entries().size(), QcMessageTally::kMaxDistinctCommands,
                  "entries must stay bounded at kMaxDistinctCommands");
    zassert_equal(tally.entries()[0].count, 2u, "an already-tracked command keeps incrementing");
    zassert_equal(tally.otherCount(), 3u, "every brand-new command past capacity falls into otherCount");
}

ZTEST(dispatch_messages, test_message_tally_tracks_peak_sizes_per_command)
{
    // issue #33: per-command peak wire/inflated size, not just count.
    QcMessageTally tally;
    tally.start();

    tally.record(4, 100, 90);
    tally.record(4, 3946, 3938);  // the peak for command 4
    tally.record(4, 50, 40);
    tally.record(15, 2408, 20480);  // a different command's own peak

    const auto entries = tally.entries();
    zassert_equal(entries.size(), 2u, "expected two distinct commands");
    zassert_equal(entries[0].command, 4u, "command 4");
    zassert_equal(entries[0].count, 3u, "command 4 count");
    zassert_equal(entries[0].maxWireBytes, 3946u,
                  "command 4 maxWireBytes should track the peak, not the last value");
    zassert_equal(entries[0].maxInflatedBytes, 3938u, "command 4 maxInflatedBytes should track the peak");
    zassert_equal(entries[1].command, 15u, "command 15");
    zassert_equal(entries[1].maxWireBytes, 2408u, "command 15 maxWireBytes");
    zassert_equal(entries[1].maxInflatedBytes, 20480u, "command 15 maxInflatedBytes");
}

ZTEST(dispatch_messages, test_message_tally_record_without_sizes_defaults_to_zero)
{
    // Existing single-argument record() calls (this suite's own tests
    // above) must keep compiling and behaving the same way.
    QcMessageTally tally;
    tally.start();
    tally.record(4);

    zassert_equal(tally.entries()[0].maxWireBytes, 0u, "default maxWireBytes");
    zassert_equal(tally.entries()[0].maxInflatedBytes, 0u, "default maxInflatedBytes");
}

// --- QcReassemblyOverflowTally.hpp (issue #33) ------------------------------

ZTEST(dispatch_messages, test_reassembly_overflow_tally_counts_and_tracks_max_per_command)
{
    QcReassemblyOverflowTally tally;

    tally.record(51, 5000);   // ModelRepo
    tally.record(51, 12000);  // same command, bigger - should bump the max
    tally.record(4, 4200);    // File

    std::array<QcReassemblyOverflowTally::Entry, QcReassemblyOverflowTally::kMaxDistinctCommands> entries{};
    const size_t count = tally.entries(entries);

    zassert_equal(count, 2u, "expected two distinct commands");
    zassert_equal(entries[0].command, 51u, "first distinct command seen, in order");
    zassert_equal(entries[0].count, 2u, "command 51 count");
    zassert_equal(entries[0].maxTrueSize, 12000u,
                  "command 51 maxTrueSize should track the peak, not the last value");
    zassert_equal(entries[1].command, 4u, "second distinct command seen, in order");
    zassert_equal(entries[1].count, 1u, "command 4 count");
    zassert_equal(entries[1].maxTrueSize, 4200u, "command 4 maxTrueSize");
    zassert_equal(tally.otherCount(), 0u, "otherCount should stay 0 within capacity");
}

ZTEST(dispatch_messages, test_reassembly_overflow_tally_overflow_falls_into_other)
{
    QcReassemblyOverflowTally tally;

    for (uint16_t command = 0; command < QcReassemblyOverflowTally::kMaxDistinctCommands; ++command) {
        tally.record(command, 5000);
    }
    tally.record(static_cast<uint16_t>(QcReassemblyOverflowTally::kMaxDistinctCommands), 6000);
    tally.record(static_cast<uint16_t>(QcReassemblyOverflowTally::kMaxDistinctCommands + 1), 9000);

    std::array<QcReassemblyOverflowTally::Entry, QcReassemblyOverflowTally::kMaxDistinctCommands> entries{};
    const size_t count = tally.entries(entries);

    zassert_equal(count, QcReassemblyOverflowTally::kMaxDistinctCommands,
                  "entries must stay bounded at kMaxDistinctCommands");
    zassert_equal(tally.otherCount(), 2u, "every brand-new command past capacity falls into otherCount");
    zassert_equal(tally.otherMaxTrueSize(), 9000u,
                  "otherMaxTrueSize should track the peak across the overflow bucket");
}

// --- QcStateReads.hpp -------------------------------------------------------

ZTEST(dispatch_messages, test_post_connect_state_read_commands_match_qc_mcp_handshake_order)
{
    // Exact order and set of qc-mcp's transport.py::_handshake() loop
    // (ModelRepo, ModuleStats, UndoRedo, IOSettings, GeneralSettings, Mode,
    // GlobalEQ, MasterVolume, GlobalTempo, Scene, PresetDirty,
    // SetlistPosition), command ids from protocol.py's COMMANDS map.
    const std::array<uint16_t, 12> expected = {51, 35, 21, 3, 9, 14, 38, 17, 33, 13, 34, 2};
    zassert_equal(kPostConnectStateReadCommands.size(), expected.size(), "expected 12 state READs");
    for (size_t i = 0; i < expected.size(); ++i) {
        zassert_equal(kPostConnectStateReadCommands[i], expected[i], "command id at index %zu", i);
    }
}

ZTEST(dispatch_messages, test_encode_state_read_model_repo)
{
    HandshakeMessageBuffer out;
    encodeStateRead(out, kCommandModelRepo, 3);

    const std::vector<uint8_t> expected = {
        0x08, 0x03,              // action = READ (3)
        0x10, 0x03,              // request_id = 3
        0x33, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=51 (ModelRepo)
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(dispatch_messages, test_encode_state_read_setlist_position)
{
    HandshakeMessageBuffer out;
    encodeStateRead(out, kCommandSetlistPosition, 5);

    const std::vector<uint8_t> expected = {
        0x08, 0x03,              // action = READ (3)
        0x10, 0x05,              // request_id = 5
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=2 (SetlistPosition)
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(dispatch_messages, test_encode_state_read_multi_byte_request_id)
{
    HandshakeMessageBuffer out;
    encodeStateRead(out, kCommandGlobalTempo, 300);  // 300 -> varint [0xAC, 0x02]

    const std::vector<uint8_t> expected = {
        0x08, 0x03,                    // action = READ (3)
        0x10, 0xAC, 0x02,              // request_id = 300
        0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // trailer: command=33 (GlobalTempo)
    };
    zassert_equal(out.size(), expected.size(), "size");
    zassert_mem_equal(out.data(), expected.data(), expected.size(), "bytes");
}

ZTEST(dispatch_messages, test_message_tally_stop_blocks_further_records_until_restart)
{
    QcMessageTally tally;
    tally.start();
    tally.record(4);
    tally.stop();

    zassert_false(tally.isActive(), "expected inactive after stop()");
    tally.record(4);
    zassert_equal(tally.entries()[0].count, 1u, "a record after stop() must be dropped");

    // A fresh start() must not carry over the previous window's counts.
    tally.start();
    zassert_equal(tally.entries().size(), 0u, "expected a clean slate after restart");
    zassert_equal(tally.otherCount(), 0u, "otherCount must reset on restart too");
}

// --- QcFileReadExperiment.hpp (issue #33 variant experiment) ---------------

namespace {

template <typename Container>
std::string_view asStringView(const Container& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Same shape as tests/file_messages's own helpers: a FolderInfo (1=key,
// 2=parent_key, 3=name, 4=is_factory, 5=is_downloads, 7=files repeated)
// wrapping ProductData entries (3=name only - this experiment, like issue
// #59's decoder, never reads a product's other fields). Deliberately not
// wrapped in a second by-value-returning helper for "the whole File
// message" - nesting two etl::vector<uint8_t, 2048>-returning functions
// overflowed the ztest thread's stack on qemu_cortex_m3 at low
// optimization (each frame reserves its own 2KB-plus locals regardless of
// NRVO); building the top-level `type`/`folder` proto directly in each
// test, one level of nesting only, mirrors file_messages's own tests,
// which are verified to fit.
etl::vector<uint8_t, 2048> buildFolder(std::string_view key, std::string_view name, bool isFactory,
                                       const std::vector<std::string_view>& presetNames,
                                       std::string_view parentKey = "", bool isDownloads = false) {
    etl::vector<uint8_t, 2048> out;
    writeString(out, 1, key);
    if (!parentKey.empty()) {
        writeString(out, 2, parentKey);
    }
    writeString(out, 3, name);
    writeBool(out, 4, isFactory);
    writeBool(out, 5, isDownloads);
    for (const auto& presetName : presetNames) {
        etl::vector<uint8_t, 512> product;
        writeString(product, 3, presetName);
        writeString(out, 7, asStringView(product));
    }
    return out;
}

etl::vector<uint8_t, 2048> buildFileMessage(int64_t type, std::string_view key, std::string_view name,
                                            bool isFactory, const std::vector<std::string_view>& presetNames,
                                            std::string_view parentKey = "", bool isDownloads = false) {
    const auto folder = buildFolder(key, name, isFactory, presetNames, parentKey, isDownloads);
    etl::vector<uint8_t, 2048> out;
    writeUInt64(out, 3, type);
    writeString(out, 4, asStringView(folder));
    return out;
}

}  // namespace

ZTEST(dispatch_messages, test_file_read_experiment_variant_table)
{
    // Order matters (QcFileReadExperiment.hpp's own comment on kVariants):
    // short_listing/omit_factory_content/both first, `default` last.
    zassert_equal(QcFileReadExperiment::kVariants.size(), 4u, "expected four variants");
    zassert_true(QcFileReadExperiment::kVariants[0].options.shortListing, "variant 0 shortListing");
    zassert_false(QcFileReadExperiment::kVariants[0].options.omitFactoryContent,
                  "variant 0 omitFactoryContent");
    zassert_false(QcFileReadExperiment::kVariants[1].options.shortListing, "variant 1 shortListing");
    zassert_true(QcFileReadExperiment::kVariants[1].options.omitFactoryContent,
                 "variant 1 omitFactoryContent");
    zassert_true(QcFileReadExperiment::kVariants[2].options.shortListing, "variant 2 shortListing");
    zassert_true(QcFileReadExperiment::kVariants[2].options.omitFactoryContent,
                 "variant 2 omitFactoryContent");
    zassert_equal(QcFileReadExperiment::kVariants[3].label, "default", "variant 3 (last) label");
    zassert_false(QcFileReadExperiment::kVariants[3].options.shortListing, "variant 3 shortListing");
    zassert_false(QcFileReadExperiment::kVariants[3].options.omitFactoryContent,
                  "variant 3 omitFactoryContent");
}

ZTEST(dispatch_messages, test_file_read_experiment_cycles_through_every_variant)
{
    QcFileReadExperiment experiment;
    experiment.reset();

    zassert_equal(experiment.variantNumber(), 1u, "expected to start at variant 1");
    zassert_false(experiment.isLastVariant(), "variant 1 of 4 is not last");

    experiment.advanceVariant();
    zassert_equal(experiment.variantNumber(), 2u, "variant 2");
    zassert_false(experiment.isLastVariant(), "variant 2 of 4 is not last");

    experiment.advanceVariant();
    zassert_equal(experiment.variantNumber(), 3u, "variant 3");
    zassert_false(experiment.isLastVariant(), "variant 3 of 4 is not last");

    experiment.advanceVariant();
    zassert_equal(experiment.variantNumber(), 4u, "variant 4");
    zassert_true(experiment.isLastVariant(), "variant 4 of 4 is last");
    zassert_true(experiment.hasCurrentVariant(), "variant 4 is still a valid current variant");

    experiment.advanceVariant();
    zassert_false(experiment.hasCurrentVariant(), "past the last variant, none is current");
}

ZTEST(dispatch_messages, test_file_read_experiment_counts_preset_folders_and_names)
{
    QcFileReadExperiment experiment;
    experiment.beginVariant();

    // one preset has an empty name - an unused slot, not a preset. Each
    // message is built and consumed in its own scope, not held alongside
    // the other two - three simultaneously-alive 2048-byte buffers plus
    // this class's own (now sizeable, with its per-folder table) locals
    // overflowed the ztest thread's stack here once already.
    {
        const auto userMsg = buildFileMessage(kFileTypePresets, "1_q", "My Presets", false,
                                              {"Lead Tone", ""});
        experiment.onFileMessage({userMsg.data(), userMsg.size()});
    }
    {
        const auto factoryMsg = buildFileMessage(kFileTypePresets, "/opt/neuraldsp/factory",
                                                 "Factory Library", true, {"Default Clean"});
        experiment.onFileMessage({factoryMsg.data(), factoryMsg.size()});
    }
    {
        const auto irMsg = buildFileMessage(1, "CIR_abc", "My IRs", false, {"Cab IR"});  // type=1
        experiment.onFileMessage({irMsg.data(), irMsg.size()});
    }

    const auto summary = experiment.contentSummary();
    zassert_equal(summary.presetFolderCount, 2u, "two type=0 folders (IRs folder doesn't count)");
    zassert_equal(summary.factoryFolderCount, 1u, "one factory folder");
    zassert_equal(summary.userFolderCount, 1u, "one user folder");
    zassert_equal(summary.totalPresetCount, 3u, "three presets total across both preset folders");
    zassert_equal(summary.emptyNamePresetCount, 1u, "one preset had an empty name");
    zassert_equal(summary.namedFactoryPresetCount, 1u, "one named factory preset");
    zassert_equal(summary.namedUserPresetCount, 1u, "one named user preset (the empty one excluded)");

    // File messages by type, independent of decodeFile()'s presets-only
    // filtering - the IRs folder (type=1) must still show up here.
    zassert_equal(summary.typeCountEntries, 2u, "two distinct types seen");
    zassert_equal(summary.typeCounts[0].type, kFileTypePresets, "first type seen");
    zassert_equal(summary.typeCounts[0].count, 2u, "two type=0 messages");
    zassert_equal(summary.typeCounts[1].type, 1, "second type seen");
    zassert_equal(summary.typeCounts[1].count, 1u, "one type=1 message");
}

ZTEST(dispatch_messages, test_file_read_experiment_no_folder_type_minus_one)
{
    // issue #33: the "ack"-shaped reply seen on real hardware - no `type`,
    // no `folder`. Must decode cleanly (no crash, no folder recorded) and
    // show up under type=-1.
    QcFileReadExperiment experiment;
    experiment.beginVariant();

    etl::vector<uint8_t, 32> proto;
    writeEnum(proto, 1, static_cast<int>(MessageAction::Update));
    writeUInt64(proto, 2, 7);  // request_id - no type, no folder

    zassert_false(QcFileReadExperiment::hasFolder({proto.data(), proto.size()}), "no folder field");

    experiment.onFileMessage({proto.data(), proto.size()});

    const auto summary = experiment.contentSummary();
    zassert_equal(summary.presetFolderCount, 0u, "no folder means no preset folder counted");
    zassert_equal(summary.typeCountEntries, 1u, "one distinct type seen");
    zassert_equal(summary.typeCounts[0].type, -1, "missing type field reads as -1");
    zassert_equal(summary.typeCounts[0].count, 1u, "one such message");
}

ZTEST(dispatch_messages, test_file_read_experiment_has_folder_true_for_real_listing_entries)
{
    const auto msg = buildFileMessage(kFileTypePresets, "1_q", "My Presets", false, {"Lead Tone"});
    zassert_true(QcFileReadExperiment::hasFolder({msg.data(), msg.size()}),
                "a real catalog entry has a folder field");
}

ZTEST(dispatch_messages, test_file_read_experiment_folder_detail_captures_metadata_and_samples)
{
    QcFileReadExperiment experiment;
    experiment.beginVariant();

    const auto msg = buildFileMessage(kFileTypePresets, "1_q", "My Presets", false,
                                      {"Lead Tone", "Clean Rhythm", ""}, "parent_1", true);
    experiment.onFileMessage({msg.data(), msg.size()});

    const auto folders = experiment.folderDetails();
    zassert_equal(folders.size(), 1u, "expected one folder recorded");
    zassert_equal(folders[0].key, "1_q", "key");
    zassert_equal(folders[0].name, "My Presets", "name");
    zassert_equal(folders[0].parentKey, "parent_1", "parentKey");
    zassert_false(folders[0].isFactory, "isFactory");
    zassert_true(folders[0].isDownloads, "isDownloads");
    zassert_equal(folders[0].namedCount, 2u, "two named presets");
    zassert_equal(folders[0].emptyCount, 1u, "one empty slot");
    zassert_equal(folders[0].sampleCount, 2u, "two samples (named only, empty slot not sampled)");
    zassert_equal(folders[0].samples[0], "Lead Tone", "first sample");
    zassert_equal(folders[0].samples[1], "Clean Rhythm", "second sample");
}

ZTEST(dispatch_messages, test_file_read_experiment_folder_detail_replaced_not_merged_on_resend)
{
    // qc-mcp's directory.py: "later messages for the same folder key
    // replace earlier ones" - a resend must reset, not add to, that key's
    // counts/samples.
    QcFileReadExperiment experiment;
    experiment.beginVariant();

    {
        const auto first = buildFileMessage(kFileTypePresets, "1_q", "My Presets", false, {"Lead Tone"});
        experiment.onFileMessage({first.data(), first.size()});
    }
    {
        const auto second = buildFileMessage(kFileTypePresets, "1_q", "My Presets", false, {"New Tone"});
        experiment.onFileMessage({second.data(), second.size()});
    }

    const auto folders = experiment.folderDetails();
    zassert_equal(folders.size(), 1u, "still one folder, not two");
    zassert_equal(folders[0].namedCount, 1u, "replaced, not accumulated");
    zassert_equal(folders[0].samples[0], "New Tone", "the later message's content wins");
}

ZTEST(dispatch_messages, test_file_read_experiment_folder_detail_samples_bounded_per_folder)
{
    QcFileReadExperiment experiment;
    experiment.beginVariant();

    const auto msg = buildFileMessage(kFileTypePresets, "1_q", "My Presets", false,
                                      {"One", "Two", "Three", "Four", "Five"});
    experiment.onFileMessage({msg.data(), msg.size()});

    const auto folders = experiment.folderDetails();
    zassert_equal(folders.size(), 1u, "one folder");
    zassert_equal(folders[0].namedCount, 5u, "all five counted as named");
    zassert_equal(folders[0].sampleCount, QcFileReadExperiment::kMaxSamplesPerFolder,
                 "samples capped at kMaxSamplesPerFolder even with more named presets than that");
}

ZTEST(dispatch_messages, test_file_read_experiment_folder_detail_bounded_with_other_count)
{
    QcFileReadExperiment experiment;
    experiment.beginVariant();

    for (size_t i = 0; i < QcFileReadExperiment::kMaxFolders + 2; ++i) {
        const std::string key = "folder_" + std::to_string(i);
        const auto msg = buildFileMessage(kFileTypePresets, key, "Folder", false, {"Preset"});
        experiment.onFileMessage({msg.data(), msg.size()});
    }

    zassert_equal(experiment.folderDetails().size(), QcFileReadExperiment::kMaxFolders,
                 "bounded at kMaxFolders");
    zassert_equal(experiment.contentSummary().otherFolderCount, 2u, "overflow folders counted");
}

ZTEST(dispatch_messages, test_file_read_experiment_begin_variant_resets_counters)
{
    QcFileReadExperiment experiment;
    experiment.beginVariant();

    const auto msg = buildFileMessage(kFileTypePresets, "1_q", "My Presets", false, {"Lead Tone"});
    experiment.onFileMessage({msg.data(), msg.size()});

    zassert_equal(experiment.contentSummary().totalPresetCount, 1u, "one preset before reset");
    zassert_equal(experiment.folderDetails().size(), 1u, "one folder before reset");

    experiment.beginVariant();
    const auto summary = experiment.contentSummary();
    zassert_equal(summary.presetFolderCount, 0u, "presetFolderCount reset");
    zassert_equal(summary.factoryFolderCount, 0u, "factoryFolderCount reset");
    zassert_equal(summary.userFolderCount, 0u, "userFolderCount reset");
    zassert_equal(summary.totalPresetCount, 0u, "totalPresetCount reset");
    zassert_equal(summary.emptyNamePresetCount, 0u, "emptyNamePresetCount reset");
    zassert_equal(summary.namedFactoryPresetCount, 0u, "namedFactoryPresetCount reset");
    zassert_equal(summary.namedUserPresetCount, 0u, "namedUserPresetCount reset");
    zassert_equal(summary.typeCountEntries, 0u, "typeCountEntries reset");
    zassert_equal(summary.otherTypeCount, 0u, "otherTypeCount reset");
    zassert_equal(summary.otherFolderCount, 0u, "otherFolderCount reset");
    zassert_equal(experiment.folderDetails().size(), 0u, "folder table reset");
}
