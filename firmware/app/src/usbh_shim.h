#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * C-safe boundary around Zephyr's USB host stack. See usbh_shim.c for why
 * this exists: subsys/usb/host's public headers don't compile as C++ in
 * Zephyr v4.4.2 (a struct field literally named `class`, and inline helpers
 * relying on C's implicit void* conversions). Nothing here should ever
 * require the rest of the firmware to include a Zephyr USB header directly.
 */

struct qc_usbh_filter {
	uint16_t vid;
	uint16_t pid;
	/*
	 * Target interface to inspect. Zephyr's USB host stack calls probe()
	 * once per *device* (not per interface) when a class matches on
	 * vid/pid alone - it hands back USBH_CLASS_IFNUM_DEVICE (255), not a
	 * real interface number. This is the interface we look up ourselves
	 * once that device-level match happens.
	 */
	uint8_t iface;
};

/*
 * iface_class/sub/proto come from the matched interface's descriptor, so
 * callers never need to touch a Zephyr USB descriptor type.
 */
struct qc_usbh_ops {
	void (*on_init)(void);
	int (*on_probe)(uint8_t iface, uint8_t iface_class, uint8_t iface_sub,
			uint8_t iface_proto);
	void (*on_removed)(void);
};

/*
 * Configures the (single) USB host class this bridge supports and starts
 * the USB host controller. Call once, before the app's main loop.
 *
 * Returns 0 on success, a negative errno from Zephyr's USB host stack on
 * failure.
 */
int qc_usbh_bridge_start(const struct qc_usbh_filter *filter, const struct qc_usbh_ops *ops);

#ifdef __cplusplus
}
#endif
