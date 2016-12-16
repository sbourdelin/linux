/*
 *  HID driver for quirky Innomedia devices
 *
 *  Copyright (c) 2016 Tomasz Kramkowski <tk@the-tk.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/module.h>

#include "hid-ids.h"

static inline u8 fixaxis(u8 bits, int shift)
{
	u8 mask = 0x3 << shift;
	u8 axis = (bits & mask) >> shift;

	/*
	 * These controllers report -2 (2) for left/up direction and -1 (3) for
	 * both up-down or left-right pressed.
	 */
	if (axis == 3)
		axis = 0;
	else if (axis == 2)
		axis = 3;

	return (bits & ~mask) | (axis << shift);
}

static int im_raw_event(struct hid_device *hdev, struct hid_report *report,
			u8 *data, int size)
{
	if (size == 3 && (data[0] == 1 || data[0] == 2)) {
		data[1] = fixaxis(data[1], 0);
		data[1] = fixaxis(data[1], 2);
	}

	return 0;
}

static const struct hid_device_id im_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_INNOMEDIA, USB_DEVICE_ID_INNEX_GENESIS_ATARI) },
	{ }
};

MODULE_DEVICE_TABLE(hid, im_devices);

static struct hid_driver im_driver = {
	.name = "innomedia",
	.id_table = im_devices,
	.raw_event = im_raw_event,
};

module_hid_driver(im_driver);

MODULE_LICENSE("GPL");
