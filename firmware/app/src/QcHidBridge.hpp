#pragma once

#include <cstdint>

#include <zephyr/logging/log.h>

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

// Claims the Quad Cortex Mini's vendor HID interface (interface 5) and
// rejects every other interface the composite device exposes.
class QcHidBridge : public UsbHostClass {
public:
    int Start() {
        return UsbHostClass::Start(kQuadCortexVid, kQuadCortexMiniPid, kQuadCortexHidInterface);
    }

protected:
    bool OnProbe(uint8_t iface, uint8_t ifaceClass, uint8_t /*ifaceSub*/,
                uint8_t /*ifaceProto*/) override {
        // iface is always kQuadCortexHidInterface here - the shim looks up
        // that specific interface before calling OnProbe at all. Still
        // worth checking the class code: a CorOS change moving what's at
        // interface 5 should make us reject it, not silently misclaim it.
        if (ifaceClass != kUsbBaseClassHid) {
            return false;
        }
        LOG_INF("Claimed Quad Cortex Mini HID interface %u", iface);
        return true;
    }

    void OnRemoved() override { LOG_INF("Quad Cortex Mini disconnected"); }
};

}  // namespace qcbridge
