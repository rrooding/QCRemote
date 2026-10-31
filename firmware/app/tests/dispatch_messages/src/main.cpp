#include <array>
#include <cstdint>
#include <vector>

#include <zephyr/ztest.h>

#include "QcFileRequest.hpp"
#include "QcFileStream.hpp"
#include "QcMessageTally.hpp"
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
