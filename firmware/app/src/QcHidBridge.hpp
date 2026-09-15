#pragma once

#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/logging/log.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbh.h>

// Internal Zephyr USB host headers, not part of the public zephyr/usb/usbh.h
// API - exposed to this app via the extra include path in CMakeLists.txt.
// Every other in-tree USB host class driver (MSC, UAC2, UVC) depends on
// these too; there's currently no public alternative. Flag it if a future
// Zephyr version moves or breaks them.
#include <usbh_class.h>
#include <usbh_desc.h>

LOG_MODULE_REGISTER(qc_hid_bridge, LOG_LEVEL_INF);

namespace qcbridge {

// Quad Cortex Mini identity and HID control interface, per qc-mcp's
// PROTOCOL.md (https://github.com/lexasoft123/qc-mcp).
inline constexpr uint16_t kQuadCortexVid = 0x152A;
inline constexpr uint16_t kQuadCortexMiniPid = 0x892F;
inline constexpr uint8_t kQuadCortexHidInterface = 5;

// USB host class that claims the Quad Cortex Mini's vendor HID interface.
// The class filter (kQcHidFilter, below) matches on VID/PID alone, which
// matches every interface the device exposes; Probe() then accepts only
// interface 5, per qc-mcp.
//
// Static-only by design: there's exactly one bridge instance, and
// USBH_DEFINE_CLASS needs file-scope registration anyway, so instance state
// lives in static members rather than a heap- or stack-allocated object.
class QcHidBridge {
private:
    enum class State { Idle, Claimed };

    static int Init(usbh_class_data*) {
        LOG_INF("QC HID bridge class initialized");
        return 0;
    }

    static int CompletionCb(usbh_class_data*, uhc_transfer*) {
        // Real transfer handling (control/interrupt I/O) lands with the HID
        // framing and session work (#4-#8). Nothing submits transfers yet.
        LOG_WRN("Unhandled transfer completion");
        return -ENOTSUP;
    }

    static int Probe(usbh_class_data*, usb_device* udev, uint8_t iface) {
        if (iface != kQuadCortexHidInterface) {
            return -ENOTSUP;
        }

        const auto* desc =
            static_cast<const usb_if_descriptor*>(usbh_desc_get_iface(udev, iface));
        if (desc == nullptr || desc->bInterfaceClass != USB_BCC_HID) {
            LOG_WRN("Interface %u is not the expected vendor HID interface", iface);
            return -ENOTSUP;
        }

        LOG_INF("Claimed Quad Cortex Mini HID interface %u", iface);
        state = State::Claimed;
        return 0;
    }

    static int Removed(usbh_class_data*) {
        LOG_INF("Quad Cortex Mini disconnected");
        state = State::Idle;
        return 0;
    }

    static inline State state = State::Idle;

public:
    static inline usbh_class_api api = {
        .init = &Init,
        .completion_cb = &CompletionCb,
        .probe = &Probe,
        .removed = &Removed,
    };
};

inline constexpr usbh_class_filter kQcHidFilter[] = {
    {
        .vid = kQuadCortexVid,
        .pid = kQuadCortexMiniPid,
        .flags = USBH_CLASS_MATCH_VID_PID,
    },
    {0},
};

}  // namespace qcbridge
