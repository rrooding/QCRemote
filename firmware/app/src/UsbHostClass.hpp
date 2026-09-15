#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Zephyr's minimal C++ library (lib/cpp/minimal) doesn't provide <cerrno>;
// the plain C header works fine from C++ since these are just macros.
#include <errno.h>

extern "C" {
#include "usbh_shim.h"
}

namespace qcbridge {

// Generic base for a USB host class, bridged through usbh_shim.c (see that
// file for why a shim exists at all: Zephyr's public USB host headers
// aren't C++-safe as of v4.4.2). Subclass this, override onProbe() (and
// optionally onInit()/onRemoved()/onReportIn()), then call
// start(vid, pid, iface).
//
// Only one instance may be started at a time - the underlying shim
// supports a single registered class, matching this project's actual need
// (one bridge, one device). Starting a second instance silently replaces
// the first's registration.
class UsbHostClass {
public:
    virtual ~UsbHostClass() = default;

    // Starts the USB host controller and registers this instance as the
    // class for devices matching vid/pid, offering interface `iface` to
    // onProbe(). Once accepted, the shim starts a continuous receive loop
    // on the interface's IN endpoint, delivering each report to
    // onReportIn(). Returns 0 on success, a negative errno from Zephyr's
    // USB host stack on failure.
    int start(uint16_t vid, uint16_t pid, uint8_t iface) {
        instance = this;
        const qc_usbh_filter filter{.vid = vid, .pid = pid, .iface = iface};
        const qc_usbh_ops ops{
            .on_init = &trampolineInit,
            .on_probe = &trampolineProbe,
            .on_removed = &trampolineRemoved,
            .on_report_in = &trampolineReportIn,
        };
        return qc_usbh_bridge_start(&filter, &ops);
    }

    // Sends one report on the claimed device's OUT endpoint. Only
    // meaningful after onProbe() has accepted a device. See
    // qc_usbh_send_report()'s doc comment for what "success" means here.
    static int sendReport(std::span<const uint8_t> report) {
        return qc_usbh_send_report(report.data(), report.size());
    }

protected:
    // Called once, before any device connects.
    virtual void onInit() {}

    // Called once a VID/PID-matched device connects, with the descriptor of
    // the `iface` passed to start(). Return true to accept the device,
    // false to reject it (e.g. the interface isn't the class you expected).
    virtual bool onProbe(uint8_t iface, uint8_t ifaceClass, uint8_t ifaceSub,
                         uint8_t ifaceProto) = 0;

    // Called when the matched device is disconnected.
    virtual void onRemoved() {}

    // Called once per received report, exactly QC_USBH_REPORT_SIZE bytes
    // (or fewer on a short packet). Default does nothing - override if you
    // actually need to receive.
    virtual void onReportIn(std::span<const uint8_t> report) { (void)report; }

private:
    static void trampolineInit() {
        if (instance != nullptr) {
            instance->onInit();
        }
    }

    static int trampolineProbe(uint8_t iface, uint8_t ifaceClass, uint8_t ifaceSub,
                               uint8_t ifaceProto) {
        if (instance == nullptr) {
            return -ENOTSUP;
        }
        return instance->onProbe(iface, ifaceClass, ifaceSub, ifaceProto) ? 0 : -ENOTSUP;
    }

    static void trampolineRemoved() {
        if (instance != nullptr) {
            instance->onRemoved();
        }
    }

    static void trampolineReportIn(const uint8_t* data, size_t len) {
        if (instance != nullptr) {
            instance->onReportIn(std::span<const uint8_t>(data, len));
        }
    }

    static inline UsbHostClass* instance = nullptr;
};

}  // namespace qcbridge
