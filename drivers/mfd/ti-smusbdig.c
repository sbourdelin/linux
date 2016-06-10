/*
 * MFD Core driver for TI SM-USB-DIG
 *
 * Copyright (C) 2016 Texas Instruments Incorporated - http://www.ti.com/
 *	Andrew F. Davis <afd@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether expressed or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License version 2 for more details.
 */

#include <linux/mfd/core.h>
#include <linux/mfd/ti-smusbdig.h>
#include <linux/module.h>
#include <linux/usb.h>

#define TI_USB_VENDOR_ID                0x0451
#define TI_USB_DEVICE_ID_SM_USB_DIG     0x2f90

#define TI_SMUSBDIG_USB_TIMEOUT_MS      1000

struct ti_smusbdig_device {
	struct usb_device *usb_dev;
	struct usb_interface *interface;
};

int ti_smusbdig_xfer(struct ti_smusbdig_device *ti_smusbdig,
		     u8 *buffer, int size)
{
	struct device *dev = &ti_smusbdig->interface->dev;
	int actual_length, ret;

	if (!ti_smusbdig || !buffer || size <= 0)
		return -EINVAL;

	ret = usb_interrupt_msg(ti_smusbdig->usb_dev,
				usb_sndctrlpipe(ti_smusbdig->usb_dev, 1),
				buffer, size, &actual_length,
				TI_SMUSBDIG_USB_TIMEOUT_MS);
	if (ret) {
		dev_err(dev, "USB transaction failed\n");
		return ret;
	}

	ret = usb_interrupt_msg(ti_smusbdig->usb_dev,
				usb_rcvctrlpipe(ti_smusbdig->usb_dev, 1),
				buffer, TI_SMUSBDIG_PACKET_SIZE, &actual_length,
				TI_SMUSBDIG_USB_TIMEOUT_MS);
	if (ret) {
		dev_err(dev, "USB transaction failed\n");
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(ti_smusbdig_xfer);

static const struct mfd_cell ti_smusbdig_mfd_cells[] = {
	{ .name = "ti-sm-usb-dig-gpio", },
	{ .name = "ti-sm-usb-dig-i2c", },
	{ .name = "ti-sm-usb-dig-spi", },
	{ .name = "ti-sm-usb-dig-w1", },
};

static int ti_smusbdig_probe(struct usb_interface *interface,
			     const struct usb_device_id *usb_id)
{
	struct usb_host_interface *hostif = interface->cur_altsetting;
	struct device *dev = &interface->dev;
	struct ti_smusbdig_device *ti_smusbdig;
	u8 buffer[TI_SMUSBDIG_PACKET_SIZE];
	int ret;

	if (hostif->desc.bInterfaceNumber != 0 ||
	    hostif->desc.bNumEndpoints < 2)
		return -ENODEV;

	ti_smusbdig = devm_kzalloc(dev, sizeof(*ti_smusbdig), GFP_KERNEL);
	if (!ti_smusbdig)
		return -ENOMEM;

	ti_smusbdig->usb_dev = usb_get_dev(interface_to_usbdev(interface));
	ti_smusbdig->interface = interface;
	usb_set_intfdata(interface, ti_smusbdig);

	buffer[0] = TI_SMUSBDIG_VERSION;
	ret = ti_smusbdig_xfer(ti_smusbdig, buffer, 1);
	if (ret)
		return ret;

	dev_info(dev, "TI SM-USB-DIG Version: %d.%02d Found\n",
		 buffer[0], buffer[1]);

	/* Turn on power supply output */
	buffer[0] = TI_SMUSBDIG_COMMAND;
	buffer[1] = TI_SMUSBDIG_COMMAND_DUTPOWERON;
	ret = ti_smusbdig_xfer(ti_smusbdig, buffer, 2);
	if (ret)
		return ret;

	dev_set_drvdata(dev, ti_smusbdig);
	ret = mfd_add_hotplug_devices(dev, ti_smusbdig_mfd_cells,
				      ARRAY_SIZE(ti_smusbdig_mfd_cells));
	if (ret) {
		dev_err(dev, "unable to add MFD devices\n");
		return ret;
	}

	return 0;
}

static void ti_smusbdig_disconnect(struct usb_interface *interface)
{
	mfd_remove_devices(&interface->dev);
}

static const struct usb_device_id ti_smusbdig_id_table[] = {
	{ USB_DEVICE(TI_USB_VENDOR_ID, TI_USB_DEVICE_ID_SM_USB_DIG) },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(usb, ti_smusbdig_id_table);

static struct usb_driver ti_smusbdig_driver = {
	.name = "ti-sm-usb-dig",
	.probe = ti_smusbdig_probe,
	.disconnect = ti_smusbdig_disconnect,
	.id_table = ti_smusbdig_id_table,
};
module_usb_driver(ti_smusbdig_driver);

MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_DESCRIPTION("Core driver for TI SM-USB-DIG interface adapter");
MODULE_LICENSE("GPL v2");
