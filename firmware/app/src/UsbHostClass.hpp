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
// aren't C++-safe as of v4.4.2). Subclass this, override onProbe() (and
// optionally onInit()/onRemoved()), then call start(vid, pid, iface).
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
    // onProbe(). Returns 0 on success, a negative errno from Zephyr's USB
    // host stack on failure.
    int start(uint16_t vid, uint16_t pid, uint8_t iface) {
        instance = this;
        const qc_usbh_filter filter{.vid = vid, .pid = pid, .iface = iface};
        const qc_usbh_ops ops{
            .on_init = &trampolineInit,
            .on_probe = &trampolineProbe,
            .on_removed = &trampolineRemoved,
        };
        return qc_usbh_bridge_start(&filter, &ops);
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

    static inline UsbHostClass* instance = nullptr;
};

}  // namespace qcbridge
