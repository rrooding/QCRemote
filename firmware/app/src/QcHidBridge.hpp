#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <zephyr/logging/log.h>

#include "HidReassembler.hpp"
#include "QcSession.hpp"
#include "UsbHostClass.hpp"

namespace qcbridge {

// Quad Cortex Mini identity and HID control interface, per qc-mcp's
// PROTOCOL.md (https://github.com/lexasoft123/qc-mcp).
inline constexpr uint16_t kQuadCortexVid = 0x152A;
inline constexpr uint16_t kQuadCortexMiniPid = 0x892F;
inline constexpr uint8_t kQuadCortexHidInterface = 5;

// USB base class code for HID (USB.org-assigned, 0x03) - defined locally
// rather than pulling in Zephyr's usb_ch9.h, so this file has no Zephyr USB
// header dependency at all.
inline constexpr uint8_t kUsbBaseClassHid = 0x03;

// Placeholder pending issue #33's actual sizing exercise - generous enough
// for any MVP1 message (Version, Connection, ResetCommsBuffers, KeepAlive,
// ModelRepo, SetlistPosition) without having measured them yet.
inline constexpr size_t kReassemblyCapacity = 4096;

// Claims the Quad Cortex Mini's vendor HID interface (interface 5), rejects
// every other interface the composite device exposes, and reassembles
// incoming HID reports into logical messages.
class QcHidBridge : public UsbHostClass {
public:
    int start() {
        return UsbHostClass::start(kQuadCortexVid, kQuadCortexMiniPid, kQuadCortexHidInterface);
    }

protected:
    bool onProbe(uint8_t iface, uint8_t ifaceClass, uint8_t ifaceSub, uint8_t ifaceProto) override {
        // iface is always kQuadCortexHidInterface here - the shim looks up
        // that specific interface before calling onProbe at all. Still
        // worth checking the class code: a CorOS change moving what's at
        // interface 5 should make us reject it, not silently misclaim it.
        if (ifaceClass != kUsbBaseClassHid) {
            return false;
        }
        LOG_INF("Claimed Quad Cortex Mini HID interface %u (sub=%u, proto=%u)", iface, ifaceSub,
                ifaceProto);
        session_.deviceConnected();
        return true;
    }

    void onRemoved() override { LOG_INF("Quad Cortex Mini disconnected"); }

    void onReportIn(std::span<const uint8_t> report) override {
        using Status = HidReassembler<kReassemblyCapacity>::Status;

        switch (reassembler_.feed(report)) {
        case Status::Complete: {
            const auto message = reassembler_.message();
            // LOG_DBG, not LOG_INF: a File stream (issue #60) reassembles
            // on the order of 400 messages over ~12s, which would drown
            // everything else logged during it at the default level.
            LOG_DBG("Reassembled message: %zu bytes", message.size());
            session_.handleMessage(message);
            break;
        }
        case Status::Overflow:
            // No LOG_WRN here (issue #60's hardware-safety follow-up): this
            // runs on the `usbh` thread, where CONFIG_LOG_MODE_IMMEDIATE
            // means an *enabled* log level (unlike the LOG_DBG above, which
            // is filtered out at the default level and so never actually
            // formats) really does run cbprintf synchronously on that
            // 1536-byte stack - the exact pattern that overflowed it once
            // already (see QcSession.hpp's handleMessage() comment). An
            // atomic counter costs nothing there; session_.recordReassemblyOverflow()
            // surfaces the count through QcFileStream's summary instead
            // (the known gap documented in firmware/README.md's issue #60
            // section).
            session_.recordReassemblyOverflow();
            break;
        case Status::Malformed:
            session_.recordReassemblyMalformed();
            break;
        case Status::InProgress:
            break;
        }
    }

private:
    HidReassembler<kReassemblyCapacity> reassembler_;
    QcSession session_;
};

}  // namespace qcbridge
