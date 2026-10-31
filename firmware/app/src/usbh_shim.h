#pragma once

#include <stddef.h>
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

/*
 * Every qc-mcp HID report is exactly this many bytes (see PROTOCOL.md) -
 * matches qcbridge::kHidReportSize (HidReassembler.hpp), which this plain-C
 * header can't include directly. Keep the two in sync by hand.
 */
#define QC_USBH_REPORT_SIZE 129

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
 *
 * on_report_in is called once per received HID report (exactly
 * QC_USBH_REPORT_SIZE bytes, or fewer on a short packet - the caller
 * decides whether that's an error). Called from the USB host stack's own
 * thread, not an ISR, but treat it as time-sensitive: do the minimum
 * necessary and return.
 */
struct qc_usbh_ops {
	void (*on_init)(void);
	int (*on_probe)(uint8_t iface, uint8_t iface_class, uint8_t iface_sub,
			uint8_t iface_proto);
	void (*on_removed)(void);
	void (*on_report_in)(const uint8_t *data, size_t len);
};

/*
 * Configures the (single) USB host class this bridge supports and starts
 * the USB host controller. Call once, before the app's main loop.
 *
 * Once a device is claimed, this also starts a continuous receive loop on
 * its IN endpoint (re-armed automatically after every report, including
 * on_report_in callbacks) - there's no separate "start receiving" call.
 *
 * Returns 0 on success, a negative errno from Zephyr's USB host stack on
 * failure.
 */
int qc_usbh_bridge_start(const struct qc_usbh_filter *filter, const struct qc_usbh_ops *ops);

/*
 * Sends one HID report to the claimed device via a SET_REPORT
 * class-specific control transfer on endpoint 0. Only valid after
 * on_probe() has accepted a device.
 *
 * The Quad Cortex Mini's interface 5 has exactly one endpoint (0x81,
 * interrupt IN) - no interrupt OUT endpoint at all, confirmed on real
 * hardware (issue #4) - so a control transfer is the only way to send
 * anything at all, not an optimization.
 *
 * Unlike the old (broken) interrupt-OUT-based version, this is a
 * *blocking* call: it waits for the control transfer's full setup/data/
 * status stage sequence to complete (Zephyr's usbh_req_setup(), up to its
 * 5s internal timeout) and returns the actual result, not just submission
 * status. Do not call this from on_report_in() or on_probe() - both run on
 * the USB host stack's own thread, the same thread that completes this
 * transfer, so calling it from either would deadlock. Call it from a
 * separate thread instead (see QcSession.hpp).
 *
 * Returns 0 on success, a negative errno otherwise (no device claimed,
 * out of memory, or whatever usbh_req_setup() itself returns - including
 * -ETIMEDOUT). Returns the real transfer result, including -EPIPE, which
 * every send to this device returns on real hardware - this function
 * doesn't swallow that itself, callers that care should (see
 * QcSession.hpp). PROTOCOL.md §1 documents a macOS quirk where
 * IOHIDDeviceSetReport returns a benign-looking error while the data still
 * lands, and #7 assumed -EPIPE here was the same thing one layer down -
 * issue #8's hardware testing put real doubt on that (see
 * firmware/README.md's KeepAlive section): usbh_req_setup() (usbh_ch9.c)
 * returns a single pass/fail result for the whole control transfer, with
 * no way to tell a status-stage-only stall apart from the data stage never
 * completing - the underlying MCUX EHCI driver only tracks bytes
 * transferred for IN-direction transfers, and this is OUT.
 */
int qc_usbh_send_report(const uint8_t *report, size_t len);

/*
 * Total IN-endpoint transfers completed so far (successes and errors both -
 * this counts in_xfer_cb() invocations, not reassembled messages). Added for
 * issue #8's hardware diagnosis: on_report_in() and qc_usbh_send_report()
 * share the USB host stack's single processing thread (see that function's
 * doc comment above), so if sends every ~200ms ever starve or otherwise
 * disrupt the continuous receive loop, this count going flat while the
 * KeepAlive send count keeps climbing is the signal - as opposed to the
 * device itself simply not sending anything, which this count can't
 * distinguish by itself but at least rules in/out "our receive path is
 * still alive at the transfer level".
 */
uint32_t qc_usbh_in_xfer_count(void);

#ifdef __cplusplus
}
#endif
