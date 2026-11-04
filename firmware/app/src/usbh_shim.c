#include "usbh_shim.h"

#include <stdbool.h>

#include <zephyr/device.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/logging/log.h>
#include <zephyr/net_buf.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbh.h>

#include <usbh_ch9.h>
#include <usbh_class.h>
#include <usbh_desc.h>
#include <usbh_device.h>

#if defined(CONFIG_QC_USBH_FORCE_FULL_SPEED)
#include <fsl_device_registers.h>

/* PORTSC1's write-1-to-clear change bits (CSC, PEC, OCC) - masked out of a
 * read-modify-write so it doesn't acknowledge changes the EHCI driver hasn't
 * handled yet. Same value as the NXP SDK's EHCI_PORTSC1_W1_BITS. */
#define QC_PORTSC1_W1C_BITS 0x0000002AU
#endif

LOG_MODULE_REGISTER(usbh_shim, LOG_LEVEL_INF);

/*
 * Compiled as plain C, not C++ - this is what actually works around the two
 * Zephyr header bugs found bringing up issue #3:
 *
 *   - struct usbh_class_filter (zephyr/usb/usbh.h) has a field literally
 *     named `class`, a reserved word in C++.
 *   - several inline helpers in zephyr/drivers/usb/uhc.h assign a `void *`
 *     (dev->data, dev->api) directly to a typed pointer, legal in C, an
 *     error in C++.
 *
 * Both are real problems in Zephyr's own public API, not anything specific
 * to this project - worth reporting upstream (tracked in issue #48).
 * usbh_shim.h is the line: the rest of the firmware talks to that C-safe
 * header and never includes a Zephyr USB header itself.
 *
 * Transfer submission (issue #4) is modeled directly on
 * subsys/usb/host/class/usbh_uvc.c's initiate_transfer/continue_transfer
 * pattern - the only in-tree, merged host class driver that actually moves
 * data, and the closest thing to a proven reference for this API.
 *
 * Sending (issue #7) uses usbh_req_setup() (subsys/usb/host/usbh_ch9.h) -
 * the same synchronous control-transfer helper Zephyr's own host stack
 * uses internally for standard requests (usbh_req_desc_dev() etc. in
 * usbh_ch9.c). That header isn't part of Zephyr's public API either, but
 * it's already reachable: firmware/app/CMakeLists.txt adds
 * ${ZEPHYR_BASE}/subsys/usb/host to the include path for this file's other
 * usbh_*.h includes above, and usbh_ch9.h lives in that same directory.
 */

static const struct qc_usbh_ops *registered_ops;
static struct qc_usbh_filter registered_filter;

static struct usb_device *claimed_udev;
static uint8_t ep_in;
static struct uhc_transfer *in_xfer;
static bool receiving;

static int shim_init(struct usbh_class_data *const c_data)
{
	(void)c_data;

	if (registered_ops != NULL && registered_ops->on_init != NULL) {
		registered_ops->on_init();
	}
	return 0;
}

static int shim_completion_cb(struct usbh_class_data *const c_data,
			      struct uhc_transfer *const xfer)
{
	(void)c_data;
	(void)xfer;

	/*
	 * Not used: every transfer we submit gets its own per-transfer
	 * callback via usbh_xfer_alloc() (see in_xfer_cb below; the send path
	 * in qc_usbh_send_report() goes through usbh_req_setup(), which
	 * manages its own internal callback), so this class-level fallback
	 * should never actually be hit.
	 */
	return -ENOTSUP;
}

/* Re-arms `xfer` with a fresh, empty receive buffer and resubmits it. */
static int arm_in_transfer(struct uhc_transfer *const xfer)
{
	struct net_buf *buf = usbh_xfer_buf_alloc(claimed_udev, QC_USBH_REPORT_SIZE);

	if (buf == NULL) {
		LOG_ERR("arm_in_transfer: usbh_xfer_buf_alloc failed");
		return -ENOMEM;
	}

	buf->len = 0;
	xfer->buf = buf;

	int ret = usbh_xfer_enqueue(claimed_udev, xfer);

	if (ret != 0) {
		LOG_ERR("arm_in_transfer: usbh_xfer_enqueue failed: %d", ret);
		net_buf_unref(buf);
	}
	return ret;
}

static int in_xfer_cb(struct usb_device *const dev, struct uhc_transfer *const xfer)
{
	(void)dev;

	struct net_buf *buf = xfer->buf;

	if (xfer->err != 0) {
		LOG_WRN("IN transfer completed with error: %d", xfer->err);
	} else {
		/*
		 * LOG_DBG, not LOG_INF (issue #60's hardware debugging): this ran
		 * on the `usbh` thread for every single report - during a File
		 * stream that's ~400 calls over ~12s, each one formatting
		 * synchronously under CONFIG_LOG_MODE_IMMEDIATE on that thread's
		 * 1536-byte stack (the same pattern that overflowed it once
		 * already - see QcSession.hpp's handleMessage() comment), and
		 * slowing reception plus swamping the log at the default level.
		 * This module registers at LOG_LEVEL_INF (see top of file), so
		 * LOG_DBG compiles out entirely here - not just runtime-filtered.
		 * QcSession now counts every successful IN transfer
		 * (QcHidBridge::onReportIn() -> QcSession::recordInTransfer(),
		 * surfaced in the File stream tally), so the per-transfer detail
		 * traded away here is still available as a count.
		 */
		LOG_DBG("IN transfer completed: %u bytes", buf->len);
		if (registered_ops != NULL && registered_ops->on_report_in != NULL) {
			registered_ops->on_report_in(buf->data, buf->len);
		}
	}

	net_buf_unref(buf);

	if (receiving) {
		int ret = arm_in_transfer(xfer);

		if (ret != 0) {
			LOG_ERR("Failed to re-arm IN transfer: %d - receive loop stopped", ret);
			receiving = false;
		}
	}

	return 0;
}

static int start_receiving(void)
{
	LOG_INF("Starting receive loop on IN endpoint 0x%02x", ep_in);

	in_xfer = usbh_xfer_alloc(claimed_udev, ep_in, in_xfer_cb, NULL);
	if (in_xfer == NULL) {
		LOG_ERR("start_receiving: usbh_xfer_alloc failed");
		return -ENOMEM;
	}

	receiving = true;

	int ret = arm_in_transfer(in_xfer);

	if (ret != 0) {
		LOG_ERR("start_receiving: initial arm failed: %d", ret);
		(void)uhc_xfer_unref(in_xfer);
		in_xfer = NULL;
		receiving = false;
	}
	return ret;
}

/* Walks the descriptors following `if_desc`, recording the first IN
 * endpoint found before the next interface descriptor. Sending (issue #7)
 * doesn't need an OUT endpoint discovered here - it goes over a control
 * transfer on endpoint 0 instead (see qc_usbh_send_report()). */
static void discover_endpoints(const struct usb_if_descriptor *const if_desc)
{
	const struct usb_desc_header *desc = (const struct usb_desc_header *)if_desc;
	int found = 0;

	ep_in = 0;

	while ((desc = usbh_desc_get_next(desc)) != NULL && found < if_desc->bNumEndpoints) {
		if (desc->bDescriptorType == USB_DESC_INTERFACE) {
			break;
		}

		if (desc->bDescriptorType == USB_DESC_ENDPOINT) {
			const struct usb_ep_descriptor *ep_desc = (const void *)desc;

			LOG_INF("Found endpoint 0x%02x, attributes 0x%02x, wMaxPacketSize %u, "
				"bInterval %u",
				ep_desc->bEndpointAddress, ep_desc->bmAttributes,
				ep_desc->wMaxPacketSize, ep_desc->bInterval);

			if (USB_EP_DIR_IS_IN(ep_desc->bEndpointAddress)) {
				ep_in = ep_desc->bEndpointAddress;
			}
			found++;
		}
	}
}

static int shim_probe(struct usbh_class_data *const c_data, struct usb_device *const udev,
		      const uint8_t iface)
{
	(void)c_data;
	/*
	 * iface is USBH_CLASS_IFNUM_DEVICE (255) here, not a real interface
	 * number - our filter is NULL/vid-pid-only, so Zephyr calls probe()
	 * once for the whole device rather than once per interface. Look up
	 * the interface we actually care about ourselves.
	 */
	(void)iface;

	if (udev->dev_desc.idVendor != registered_filter.vid ||
	    udev->dev_desc.idProduct != registered_filter.pid) {
		return -ENOTSUP;
	}

	const struct usb_if_descriptor *desc = usbh_desc_get_iface(udev, registered_filter.iface);

	if (desc == NULL) {
		return -ENOENT;
	}

	if (registered_ops == NULL || registered_ops->on_probe == NULL) {
		return -ENOTSUP;
	}

	int accepted = registered_ops->on_probe(registered_filter.iface, desc->bInterfaceClass,
						desc->bInterfaceSubClass,
						desc->bInterfaceProtocol);
	if (accepted != 0) {
		return accepted;
	}

	claimed_udev = udev;
#if defined(CONFIG_QC_USBH_FORCE_FULL_SPEED)
	/* PSPD: 0 = Full, 1 = Low, 2 = High Speed */
	LOG_INF("Port speed (PSPD): %u",
		(unsigned int)((USBOTG->PORTSC1 & USBC_PORTSC1_PSPD_MASK) >>
			       USBC_PORTSC1_PSPD_SHIFT));
#endif
	discover_endpoints(desc);

	LOG_INF("Interface %u endpoints: IN=0x%02x", registered_filter.iface, ep_in);

	if (ep_in == 0) {
		LOG_ERR("No IN endpoint found on interface %u (bNumEndpoints=%u)",
			registered_filter.iface, desc->bNumEndpoints);
		claimed_udev = NULL;
		return -ENODEV;
	}

	int ret = start_receiving();

	if (ret != 0) {
		LOG_ERR("start_receiving failed: %d", ret);
	}
	return ret;
}

static int shim_removed(struct usbh_class_data *const c_data)
{
	(void)c_data;

	receiving = false;
	claimed_udev = NULL;
	ep_in = 0;
	in_xfer = NULL;

	if (registered_ops != NULL && registered_ops->on_removed != NULL) {
		registered_ops->on_removed();
	}
	return 0;
}

static struct usbh_class_api shim_api = {
	.init = shim_init,
	.completion_cb = shim_completion_cb,
	.probe = shim_probe,
	.removed = shim_removed,
};

/*
 * NULL filter matches every device ("A filter set to NULL always matches",
 * per usbh_class.h) - the real VID/PID gate happens in shim_probe() above,
 * since usbh_class_filter itself can't be named from C++ (see top of file).
 */
USBH_DEFINE_CLASS(qc_usbh_class, &shim_api, NULL, NULL);

USBH_CONTROLLER_DEFINE(qc_usbh_ctx, DEVICE_DT_GET(DT_NODELABEL(zephyr_uhc0)));

int qc_usbh_bridge_start(const struct qc_usbh_filter *filter, const struct qc_usbh_ops *ops)
{
	registered_filter = *filter;
	registered_ops = ops;

	int ret = usbh_init(&qc_usbh_ctx);

	if (ret != 0) {
		return ret;
	}
	ret = usbh_enable(&qc_usbh_ctx);

#if defined(CONFIG_QC_USBH_FORCE_FULL_SPEED)
	/* After usbh_enable(): the controller start-up rewrites PORTSC1. The
	 * driver's later port reset is a read-modify-write, so PFSC survives it.
	 * Plug the device in after this line is logged. */
	if (ret == 0) {
		USBOTG->PORTSC1 = (USBOTG->PORTSC1 & ~QC_PORTSC1_W1C_BITS) |
				  USBC_PORTSC1_PFSC_MASK;
		LOG_WRN("USB host forced to Full Speed (CONFIG_QC_USBH_FORCE_FULL_SPEED)");
	}
#endif
	return ret;
}

int qc_usbh_send_report(const uint8_t *report, size_t len)
{
	/* A local copy, not claimed_udev itself: usbh_req_setup() below blocks
	 * (up to its own timeout), and shim_removed() can clear claimed_udev on
	 * the usbh thread meanwhile. Freeing the buffer through the cleared
	 * global then dereferenced NULL - a KeepAlive send that was in flight
	 * when the Quad Cortex disconnected crashed this way on the ESP32-S3
	 * (issue #76). */
	struct usb_device *const udev = claimed_udev;

	if (udev == NULL) {
		return -ENODEV;
	}

	struct net_buf *buf = usbh_xfer_buf_alloc(udev, len);

	if (buf == NULL) {
		return -ENOMEM;
	}
	net_buf_add_mem(buf, report, len);

	/* Host->device | Class | Interface - the standard bmRequestType for a
	 * HID class-specific request targeting an interface. */
	const uint8_t bm_request_type = (USB_REQTYPE_DIR_TO_DEVICE << 7) |
					 (USB_REQTYPE_TYPE_CLASS << 5) | USB_REQTYPE_RECIPIENT_INTERFACE;
	const uint8_t b_request = 0x09; /* HID SET_REPORT */
	/* Report type 2 = Output. Report ID matches the byte HidChunker
	 * already wrote as report[0] - HID's numbered-report convention
	 * duplicates it here in wValue's low byte too. */
	const uint8_t report_id = len > 0 ? report[0] : 0;
	const uint16_t w_value = (0x02 << 8) | report_id;
	const uint16_t w_index = registered_filter.iface;

	int ret = usbh_req_setup(udev, bm_request_type, b_request, w_value, w_index,
				 (uint16_t)len, buf);

	usbh_xfer_buf_free(udev, buf);
	return ret;
}
