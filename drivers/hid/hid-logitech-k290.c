// SPDX-License-Identifier: GPL-2.0
/*
 * HID driver for Logitech K290 keyboard
 *
 * Copyright (c) 2018 Florent Flament
 *
 * This drivers allows to configure the K290 keyboard's function key
 * behaviour (whether function mode is activated or not by default).
 *
 * Logitech custom commands taken from Marcus Ilgner k290-fnkeyctl
 * (https://github.com/milgner/k290-fnkeyctl):
 * K290_SET_FUNCTION_CMD
 * K290_SET_FUNCTION_VAL
 * K290_SET_FUNCTION_OFF
 * K290_SET_FUNCTION_ON
 *
 * Based on hid-accutouch.c and hid-elo.c
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>
#include <linux/stat.h>
#include <linux/types.h>
#include <linux/usb.h>

#include "hid-ids.h"
#include "usbhid/usbhid.h"

// Logitech K290 custom USB command and value to setup function key
#define K290_SET_FUNCTION_CMD 0x02
#define K290_SET_FUNCTION_VAL 0x001a

// Have function mode turned off (as with standard keyboards)
#define K290_SET_FUNCTION_OFF 0x0001
// Have function mode turned on (default k290 behavior)
#define K290_SET_FUNCTION_ON  0x0000

// Function key default mode is set at module load time for every K290
// keyboards plugged on the machine. By default fn_mode = 1, i.e
// sending K290_SET_FUNCTION_ON (default K290 behavior).
static bool fn_mode = 1;
module_param(fn_mode, bool, 0444);
MODULE_PARM_DESC(fn_mode, "Logitech K290 function key mode (default = 1)");

static void k290_set_function(struct usb_device *dev, uint16_t function_mode)
{
	int ret;

	ret = usb_control_msg(dev, usb_sndctrlpipe(dev, 0),
			      K290_SET_FUNCTION_CMD,
			      USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
			      K290_SET_FUNCTION_VAL,
			      function_mode, 0, 0, USB_CTRL_SET_TIMEOUT);

	if (ret < 0)
		dev_err(&dev->dev,
			"Failed to setup K290 function key, error %d\n", ret);
}

static int k290_set_function_hid_device(struct hid_device *hid)
{
	struct usb_device *usb_dev = hid_to_usb_dev(hid);

	k290_set_function(usb_dev,
			fn_mode ? K290_SET_FUNCTION_ON : K290_SET_FUNCTION_OFF);
	return 0;
}

static int k290_input_configured(struct hid_device *hid,
				 struct hid_input *hidinput)
{
	return k290_set_function_hid_device(hid);
}

static int k290_resume(struct hid_device *hid)
{
	return k290_set_function_hid_device(hid);
}

static const struct hid_device_id k290_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH,
			 USB_DEVICE_ID_LOGITECH_KEYBOARD_K290) },
	{ }
};
MODULE_DEVICE_TABLE(hid, k290_devices);

static struct hid_driver k290_driver = {
	.name = "hid-logitech-k290",
	.id_table = k290_devices,
	.input_configured = k290_input_configured,
	.resume = k290_resume,
	.reset_resume = k290_resume,
};

module_hid_driver(k290_driver);

MODULE_AUTHOR("Florent Flament <contact@florentflament.com>");
MODULE_DESCRIPTION("Logitech K290 keyboard driver");
MODULE_LICENSE("GPL v2");
