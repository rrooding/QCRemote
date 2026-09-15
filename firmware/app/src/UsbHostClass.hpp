#pragma once

#include <cstdint>

// Zephyr's minimal C++ library (lib/cpp/minimal) doesn't provide <cerrno>;
// the plain C header works fine from C++ since these are just macros.
#include <errno.h>

extern "C" {
#include "usbh_shim.h"
}

namespace qcbridge {

// Generic base for a USB host class, bridged through usbh_shim.c (see that
// file for why a shim exists at all: Zephyr's public USB host headers
// aren't C++-safe as of v4.4.2). Subclass this, override OnProbe() (and
// optionally OnInit()/OnRemoved()), then call Start(vid, pid).
//
// Only one instance may be started at a time - the underlying shim
// supports a single registered class, matching this project's actual need
// (one bridge, one device). Starting a second instance silently replaces
// the first's registration.
class UsbHostClass {
public:
    virtual ~UsbHostClass() = default;

    // Starts the USB host controller and registers this instance as the
    // class every connected device's interfaces get offered to. Returns 0
    // on success, a negative errno from Zephyr's USB host stack on failure.
    int Start(uint16_t vid, uint16_t pid) {
        instance = this;
        const qc_usbh_filter filter{.vid = vid, .pid = pid};
        const qc_usbh_ops ops{
            .on_init = &TrampolineInit,
            .on_probe = &TrampolineProbe,
            .on_removed = &TrampolineRemoved,
        };
        return qc_usbh_bridge_start(&filter, &ops);
    }

protected:
    // Called once, before any device connects.
    virtual void OnInit() {}

    // Called once per interface of a VID/PID-matched device. Return true
    // to claim this interface, false to leave it unclaimed.
    virtual bool OnProbe(uint8_t iface, uint8_t ifaceClass, uint8_t ifaceSub,
                         uint8_t ifaceProto) = 0;

    // Called when the matched device is disconnected.
    virtual void OnRemoved() {}

private:
    static void TrampolineInit() {
        if (instance != nullptr) {
            instance->OnInit();
        }
    }

    static int TrampolineProbe(uint8_t iface, uint8_t ifaceClass, uint8_t ifaceSub,
                               uint8_t ifaceProto) {
        if (instance == nullptr) {
            return -ENOTSUP;
        }
        return instance->OnProbe(iface, ifaceClass, ifaceSub, ifaceProto) ? 0 : -ENOTSUP;
    }

    static void TrampolineRemoved() {
        if (instance != nullptr) {
            instance->OnRemoved();
        }
    }

    static inline UsbHostClass* instance = nullptr;
};

}  // namespace qcbridge
