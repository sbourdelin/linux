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

#include <linux/usb.h>
#include <linux/mfd/core.h>
#include <linux/module.h>

#include <linux/mfd/sm-usb-dig.h>

#define USB_VENDOR_ID_TI                0x0451
#define USB_DEVICE_ID_TI_SM_USB_DIG	0x2f90

#define SMUSBDIG_USB_TIMEOUT		1000	/* in ms */

struct smusbdig_device {
	struct usb_device *usb_dev;
	struct usb_interface *interface;
};

int smusbdig_xfer(struct smusbdig_device *smusbdig, u8 *buffer, int size)
{
	struct device *dev = &smusbdig->interface->dev;
	int actual_length, ret;

	if (!smusbdig || !buffer || size <= 0)
		return -EINVAL;

	ret = usb_interrupt_msg(smusbdig->usb_dev,
				usb_sndctrlpipe(smusbdig->usb_dev, 1),
				buffer, size, &actual_length,
				SMUSBDIG_USB_TIMEOUT);
	if (ret) {
		dev_err(dev, "USB transaction failed\n");
		return ret;
	}

	ret = usb_interrupt_msg(smusbdig->usb_dev,
				usb_rcvctrlpipe(smusbdig->usb_dev, 1),
				buffer, SMUSBDIG_PACKET_SIZE, &actual_length,
				SMUSBDIG_USB_TIMEOUT);
	if (ret) {
		dev_err(dev, "USB transaction failed\n");
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(smusbdig_xfer);

static const struct mfd_cell smusbdig_mfd_cells[] = {
	{ .name = "sm-usb-dig-gpio", },
	{ .name = "sm-usb-dig-i2c", },
	{ .name = "sm-usb-dig-spi", },
	{ .name = "sm-usb-dig-w1", },
};

static int smusbdig_probe(struct usb_interface *interface,
			  const struct usb_device_id *usb_id)
{
	struct usb_host_interface *hostif = interface->cur_altsetting;
	struct device *dev = &interface->dev;
	struct smusbdig_device *smusbdig;
	u8 buffer[SMUSBDIG_PACKET_SIZE];
	int ret;

	if (hostif->desc.bInterfaceNumber != 0 ||
	    hostif->desc.bNumEndpoints < 2)
		return -ENODEV;

	smusbdig = devm_kzalloc(dev, sizeof(*smusbdig), GFP_KERNEL);
	if (!smusbdig)
		return -ENOMEM;

	smusbdig->usb_dev = usb_get_dev(interface_to_usbdev(interface));
	smusbdig->interface = interface;
	usb_set_intfdata(interface, smusbdig);

	buffer[0] = SMUSBDIG_VERSION;
	ret = smusbdig_xfer(smusbdig, buffer, 1);
	if (ret)
		return ret;

	dev_info(dev, "TI SM-USB-DIG Version: %d.%02d Found\n",
		 buffer[0], buffer[1]);

	/* Turn on power supply output */
	buffer[0] = SMUSBDIG_COMMAND;
	buffer[1] = SMUSBDIG_COMMAND_DUTPOWERON;
	ret = smusbdig_xfer(smusbdig, buffer, 2);
	if (ret)
		return ret;

	dev_set_drvdata(dev, smusbdig);
	ret = mfd_add_hotplug_devices(dev, smusbdig_mfd_cells,
				      ARRAY_SIZE(smusbdig_mfd_cells));
	if (ret) {
		dev_err(dev, "unable to add MFD devices\n");
		return ret;
	}

	return 0;
}

void smusbdig_disconnect(struct usb_interface *interface)
{
	mfd_remove_devices(&interface->dev);
}

static const struct usb_device_id smusbdig_id_table[] = {
	{ USB_DEVICE(USB_VENDOR_ID_TI, USB_DEVICE_ID_TI_SM_USB_DIG) },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(usb, smusbdig_id_table);

static struct usb_driver smusbdig_driver = {
	.name = "sm-usb-dig",
	.probe = smusbdig_probe,
	.disconnect = smusbdig_disconnect,
	.id_table = smusbdig_id_table,
};
module_usb_driver(smusbdig_driver);

MODULE_AUTHOR("Andrew F. Davis <afd@ti.com>");
MODULE_DESCRIPTION("Core driver for TI SM-USB-DIG interface adapter");
MODULE_LICENSE("GPL v2");
