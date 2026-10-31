#include <cstdint>
#include <vector>

#include <zephyr/ztest.h>

#include "QcFileRequest.hpp"
#include "QcFileStream.hpp"

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
