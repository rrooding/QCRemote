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

// issue #33's final sizing (ADR 0007): must hold a worst-case File folder
// message, not just the owner's own 10193-byte `My Presets` (21 named, 235
// empty slots) under `omit_factory_content=true`. A folder maxes out at 256
// slots (confirmed on hardware: My Presets and Practice are both 256), and
// a fully-named one costs far more per slot than an empty one (qc-mcp's
// ProductData has 21 fields - name, author, tags, dates, cloud/version ids,
// dependencies, ... - vs. an empty slot's bare index). 131072 bytes
// (128 KiB) budgets ~511 bytes/slot for 256 named presets, comfortably above
// the ~414 bytes/named-slot the real 10193-byte message implies once the
// 235 near-empty slots' own small cost is subtracted - see ADR 0007 for the
// full arithmetic and the RAM cost this carries.
inline constexpr size_t kReassemblyCapacity = 131072;

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

        // Every call here is one successful IN transfer (usbh_shim.c only
        // invokes on_report_in when xfer->err == 0) - counted regardless of
        // what the reassembler does with it, so a File stream's summary can
        // tell "the device sent nothing at the USB level" apart from "it
        // sent reports that never became a counted message" (issue #60's
        // hardware debugging).
        session_.recordInTransfer();

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
            // already (see QcSession.hpp's handleMessage() comment). Atomic
            // counters cost nothing there; session_.recordReassemblyOverflow()
            // surfaces them (count + largest size seen) through the
            // heartbeat thread instead - see QcSession.hpp's
            // checkTrafficHealth().
            session_.recordReassemblyOverflow(reassembler_.overflowSize());
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
