#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace qcbridge {

// Generic message-dispatch seam (issue #60). QcDispatcher.hpp and
// QcGzipInflater.hpp both land decoded messages here, each from its own
// worker thread - never from Zephyr's `usbh` thread, see
// QcSession::handleMessage() for why that thread can't do this work itself.
// QcSession implements this.
class QcMessageSink {
public:
    virtual ~QcMessageSink() = default;

    // A message ready to handle: `protoBytes` is the plain protobuf payload
    // (already inflated, if it needed to be - QcGzipInflater.hpp did that on
    // its own thread before calling this). `wireSize` is the full
    // reassembled HID message size (QcSession::handleMessage()'s input);
    // `inflatedSize` is protoBytes.size() either way - it's just computed
    // upstream of here differently depending on `compressed`.
    virtual void onMessage(uint16_t command, std::span<const uint8_t> protoBytes, size_t wireSize,
                           size_t inflatedSize, bool compressed) = 0;

    // A gzip-compressed message (QcGzipInflater.hpp) that failed to
    // inflate - still real traffic for `command`, just with no usable
    // protoBytes to hand to onMessage(). `reason` is gzip::toString()'s
    // output (QcGzip.hpp). `trueInflatedSize` is the gzip trailer's own
    // ISIZE field (QcGzip.hpp's peekDecompressedSize(), issue #33) when
    // the failure left enough of the compressed bytes to read it from -
    // in particular, always present for `reason == "OutputTooLarge"`,
    // since that's read straight from the trailer without needing to
    // finish decompressing at all. std::nullopt if unavailable (e.g. the
    // input was truncated before the trailer).
    virtual void onInflateFailed(uint16_t command, size_t wireSize, const char* reason,
                                  std::optional<size_t> trueInflatedSize) = 0;
};

}  // namespace qcbridge
