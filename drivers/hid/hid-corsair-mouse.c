/*
 * HID driver for Corsair mouse devices
 *
 * Supported devices:
 *  - Scimitar RGB Pro
 *
 * Copyright (c) 2017 Oscar Campos
 */

/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <linux/hid.h>
#include <linux/module.h>
#include <linux/usb.h>

#include "hid-ids.h"

/*
 * The report descriptor of Corsair Scimitar RGB Pro gaming mouse is
 * non parseable as they define two consecutive Logical Minimum for
 * the Usage Page (Consumer) in rdescs bytes 75 and 77 being 77 0x16
 * that should be obviousy 0x26 for Logical Magimum of 16 bits. This
 * prevents poper parsing of the report descriptor due Logical
 * Minimum being larger than Logical Maximum.
 *
 * This driver fixes the report descriptor for:
 * - USB ID b1c:1b3e, sold as Scimitar RGB Pro Gaming mouse
 */

static __u8 *corsair_mouse_report_fixup(struct hid_device *hdev, __u8 *rdesc,
		unsigned int *rsize)
{
	struct usb_interface *intf = to_usb_interface(hdev->dev.parent);

	if (intf->cur_altsetting->desc.bInterfaceNumber == 1) {
		/*
		 * Corsair Scimitar RGB Pro report descriptor is broken and
		 * defines two different Logical Minimum for the Consumer
		 * Application. The byte 77 should be a 0x26 defining a 16
		 * bits integer for the Logical Maximum but it is a 0x16
		 * instead (Logical Minimum)
		 */
		switch (hdev->product) {
		case USB_DEVICE_ID_CORSAIR_SCIMITAR_PRO_RGB:
			if (*rsize >= 172 && rdesc[75] == 0x15 && rdesc[77] == 0x16
					&& rdesc[78] == 0xff && rdesc[79] == 0x0f) {
				hid_info(hdev, "Fixing up report descriptor\n");
				rdesc[77] = 0x26;
			}
			break;
		}

	}
	return rdesc;
}

static const struct hid_device_id corsair_mouse_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_CORSAIR,
			USB_DEVICE_ID_CORSAIR_SCIMITAR_PRO_RGB) },
	{ }
};
MODULE_DEVICE_TABLE(hid, corsair_mouse_devices);

static struct hid_driver corsair_mouse_driver = {
	.name = "corsair_mouse",
	.id_table = corsair_mouse_devices,
	.report_fixup = corsair_mouse_report_fixup,
};

module_hid_driver(corsair_mouse_driver);
MODULE_LICENSE("GPL");
