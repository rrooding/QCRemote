#include "usbh_shim.h"

#include <zephyr/device.h>
#include <zephyr/drivers/usb/uhc.h>
#include <zephyr/usb/usbh.h>

#include <usbh_class.h>
#include <usbh_desc.h>

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
 * to this project - worth reporting upstream. usbh_shim.h is the line: the
 * rest of the firmware talks to that C-safe header and never includes a
 * Zephyr USB header itself.
 */

static const struct qc_usbh_ops *registered_ops;
static struct qc_usbh_filter registered_filter;

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

	/* Transfer submission lands with the HID framing work (#4-#8). */
	return -ENOTSUP;
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

	return registered_ops->on_probe(registered_filter.iface, desc->bInterfaceClass,
					 desc->bInterfaceSubClass, desc->bInterfaceProtocol);
}

static int shim_removed(struct usbh_class_data *const c_data)
{
	(void)c_data;

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
	return usbh_enable(&qc_usbh_ctx);
}
