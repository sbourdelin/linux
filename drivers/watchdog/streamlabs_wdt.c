/*
 * StreamLabs USB Watchdog driver
 *
 * Copyright (c) 2016-2017 Alexey Klimov <klimov.linux@gmail.com>
 *
 * This program is free software; you may redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/usb.h>
#include <linux/watchdog.h>
#include <asm/byteorder.h>

/*
 * USB Watchdog device from Streamlabs:
 * https://www.stream-labs.com/en/catalog/?cat_id=1203&item_id=323
 *
 * USB commands have been reverse engineered using usbmon.
 */

#define DRIVER_AUTHOR "Alexey Klimov <klimov.linux@gmail.com>"
#define DRIVER_DESC "StreamLabs USB watchdog driver"
#define DRIVER_NAME "usb_streamlabs_wdt"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

#define USB_STREAMLABS_WATCHDOG_VENDOR	0x13c0
#define USB_STREAMLABS_WATCHDOG_PRODUCT	0x0011

/*
 * one buffer is used for communication, however transmitted message is only
 * 32 bytes long
 */
#define BUFFER_TRANSFER_LENGTH	32
#define BUFFER_LENGTH		64
#define USB_TIMEOUT		350

#define STREAMLABS_CMD_START	0xaacc
#define STREAMLABS_CMD_STOP	0xbbff

/* timeouts values are taken from windows program */
#define STREAMLABS_WDT_MIN_TIMEOUT	1
#define STREAMLABS_WDT_MAX_TIMEOUT	46

struct streamlabs_wdt {
	struct watchdog_device wdt_dev;
	struct usb_interface *intf;

	struct mutex lock;
	u8 *buffer;
};

static bool nowayout = WATCHDOG_NOWAYOUT;

/*
 * This function is used to check if watchdog actually changed
 * its state to disabled that is reported in first two bytes of response
 * message.
 */
static int usb_streamlabs_wdt_check_stop(u16 *buf)
{
	if (buf[0] != cpu_to_le16(STREAMLABS_CMD_STOP))
		return -EINVAL;

	return 0;
}

static int usb_streamlabs_wdt_validate_response(u8 *buf)
{
	/*
	 * If watchdog device understood the command it will acknowledge
	 * with values 1,2,3,4 at indexes 10, 11, 12, 13 in response message
	 * when response treated as 8bit message.
	 */
	if (buf[10] != 1 || buf[11] != 2 || buf[12] != 3 || buf[13] != 4)
		return -EINVAL;

	return 0;
}

static void usb_streamlabs_wdt_prepare_buf(u16 *buf, u16 cmd,
						unsigned long timeout_msec)
{
	/*
	 * remaining elements expected to be zero everytime during
	 * communication
	 */
	buf[0] = cpu_to_le16(cmd);
	buf[1] = cpu_to_le16(0x8000);
	buf[3] = cpu_to_le16(timeout_msec);
	buf[5] = 0x0;
	buf[6] = 0x0;
}

static int usb_streamlabs_wdt_cmd(struct streamlabs_wdt *wdt, u16 cmd)
{
	struct usb_device *usbdev;
	unsigned long timeout_msec;
	int retval;
	int size;

	if (unlikely(!wdt->intf))
		return -ENODEV;

	usbdev = interface_to_usbdev(wdt->intf);
	timeout_msec = wdt->wdt_dev.timeout * MSEC_PER_SEC;

	usb_streamlabs_wdt_prepare_buf((u16 *) wdt->buffer, cmd, timeout_msec);

	/* send command to watchdog */
	retval = usb_interrupt_msg(usbdev, usb_sndintpipe(usbdev, 0x02),
					wdt->buffer, BUFFER_TRANSFER_LENGTH,
					&size, USB_TIMEOUT);
	if (retval)
		return retval;

	if (size != BUFFER_TRANSFER_LENGTH)
		return -EIO;

	/* and read response from watchdog */
	retval = usb_interrupt_msg(usbdev, usb_rcvintpipe(usbdev, 0x81),
					wdt->buffer, BUFFER_LENGTH,
					&size, USB_TIMEOUT);
	if (retval)
		return retval;

	if (size != BUFFER_LENGTH)
		return -EIO;

	/* check if watchdog actually acked/recognized command */
	return usb_streamlabs_wdt_validate_response(wdt->buffer);
}

static int usb_streamlabs_wdt_stop_cmd(struct streamlabs_wdt *wdt)
{
	int retry_counter = 10; /* how many times to re-send stop cmd */
	int retval;

	/*
	 * Transition from enabled to disabled state in this device
	 * for stop command doesn't happen immediately. Usually, 2 or 3
	 * (sometimes even 4) stop commands should be sent until
	 * watchdog answers 'I'm stopped!'.
	 * Retry only stop command if watchdog fails to answer correctly
	 * about its state. After 10 attempts go out and return error.
	 */

	do {
		retval = usb_streamlabs_wdt_cmd(wdt, STREAMLABS_CMD_STOP);
		if (retval)
			break;

		retval = usb_streamlabs_wdt_check_stop((u16 *) wdt->buffer);

	} while (retval && --retry_counter >= 0);

	return retry_counter > 0 ? retval : -EIO;
}

static int usb_streamlabs_wdt_start(struct watchdog_device *wdt_dev)
{
	struct streamlabs_wdt *streamlabs_wdt = watchdog_get_drvdata(wdt_dev);
	int retval;

	mutex_lock(&streamlabs_wdt->lock);
	retval = usb_streamlabs_wdt_cmd(streamlabs_wdt, STREAMLABS_CMD_START);
	mutex_unlock(&streamlabs_wdt->lock);

	return retval;
}

static int usb_streamlabs_wdt_stop(struct watchdog_device *wdt_dev)
{
	struct streamlabs_wdt *streamlabs_wdt = watchdog_get_drvdata(wdt_dev);
	int retval;

	mutex_lock(&streamlabs_wdt->lock);
	retval = usb_streamlabs_wdt_stop_cmd(streamlabs_wdt);
	mutex_unlock(&streamlabs_wdt->lock);

	return retval;
}

static const struct watchdog_info streamlabs_wdt_ident = {
	.options	= WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING,
	.identity	= DRIVER_NAME,
};

static struct watchdog_ops usb_streamlabs_wdt_ops = {
	.owner	= THIS_MODULE,
	.start	= usb_streamlabs_wdt_start,
	.stop	= usb_streamlabs_wdt_stop,
};

static int usb_streamlabs_wdt_probe(struct usb_interface *intf,
					const struct usb_device_id *id)
{
	struct usb_device *usbdev = interface_to_usbdev(intf);
	struct streamlabs_wdt *streamlabs_wdt;
	int retval;

	/*
	 * USB IDs of this device appear to be weird/unregistered. Hence, do
	 * an additional check on product and manufacturer.
	 * If there is similar device in the field with same values then
	 * there is stop command in probe() below that checks if the device
	 * behaves as a watchdog.
	 */
	if (!usbdev->product || !usbdev->manufacturer
		|| strncmp(usbdev->product, "USBkit", 6)
		|| strncmp(usbdev->manufacturer, "STREAM LABS", 11))
		return -ENODEV;

	streamlabs_wdt = devm_kzalloc(&intf->dev, sizeof(struct streamlabs_wdt),
								GFP_KERNEL);
	if (!streamlabs_wdt)
		return -ENOMEM;

	streamlabs_wdt->buffer = kzalloc(BUFFER_LENGTH, GFP_KERNEL);
	if (!streamlabs_wdt->buffer)
		return -ENOMEM;

	mutex_init(&streamlabs_wdt->lock);

	streamlabs_wdt->wdt_dev.info = &streamlabs_wdt_ident;
	streamlabs_wdt->wdt_dev.ops = &usb_streamlabs_wdt_ops;
	streamlabs_wdt->wdt_dev.timeout = STREAMLABS_WDT_MAX_TIMEOUT;
	streamlabs_wdt->wdt_dev.max_timeout = STREAMLABS_WDT_MAX_TIMEOUT;
	streamlabs_wdt->wdt_dev.min_timeout = STREAMLABS_WDT_MIN_TIMEOUT;
	streamlabs_wdt->wdt_dev.parent = &intf->dev;

	streamlabs_wdt->intf = intf;
	usb_set_intfdata(intf, &streamlabs_wdt->wdt_dev);
	watchdog_set_drvdata(&streamlabs_wdt->wdt_dev, streamlabs_wdt);
	watchdog_set_nowayout(&streamlabs_wdt->wdt_dev, nowayout);

	retval = usb_streamlabs_wdt_stop(&streamlabs_wdt->wdt_dev);
	if (retval)
		goto free_buf;

	retval = watchdog_register_device(&streamlabs_wdt->wdt_dev);
	if (retval) {
		dev_err(&intf->dev, "failed to register watchdog device\n");
		goto free_buf;
	}

	dev_info(&intf->dev, "StreamLabs USB watchdog loaded.\n");
	return 0;

free_buf:
	kfree(streamlabs_wdt->buffer);
	return retval;
}

static int usb_streamlabs_wdt_suspend(struct usb_interface *intf,
					pm_message_t message)
{
	struct streamlabs_wdt *streamlabs_wdt = usb_get_intfdata(intf);

	if (watchdog_active(&streamlabs_wdt->wdt_dev))
		return usb_streamlabs_wdt_stop(&streamlabs_wdt->wdt_dev);

	return 0;
}

static int usb_streamlabs_wdt_resume(struct usb_interface *intf)
{
	struct streamlabs_wdt *streamlabs_wdt = usb_get_intfdata(intf);

	if (watchdog_active(&streamlabs_wdt->wdt_dev))
		return usb_streamlabs_wdt_start(&streamlabs_wdt->wdt_dev);

	return 0;
}

static void usb_streamlabs_wdt_disconnect(struct usb_interface *intf)
{
	struct streamlabs_wdt *streamlabs_wdt = usb_get_intfdata(intf);

	mutex_lock(&streamlabs_wdt->lock);
	usb_streamlabs_wdt_stop_cmd(streamlabs_wdt);
	usb_set_intfdata(intf, NULL);
	streamlabs_wdt->intf = NULL;
	mutex_unlock(&streamlabs_wdt->lock);

	watchdog_unregister_device(&streamlabs_wdt->wdt_dev);
}

static struct usb_device_id usb_streamlabs_wdt_device_table[] = {
	{ USB_DEVICE(USB_STREAMLABS_WATCHDOG_VENDOR, USB_STREAMLABS_WATCHDOG_PRODUCT) },
	{ }	/* Terminating entry */
};

MODULE_DEVICE_TABLE(usb, usb_streamlabs_wdt_device_table);

static struct usb_driver usb_streamlabs_wdt_driver = {
	.name		= DRIVER_NAME,
	.probe		= usb_streamlabs_wdt_probe,
	.disconnect	= usb_streamlabs_wdt_disconnect,
	.suspend	= usb_streamlabs_wdt_suspend,
	.resume		= usb_streamlabs_wdt_resume,
	.reset_resume	= usb_streamlabs_wdt_resume,
	.id_table	= usb_streamlabs_wdt_device_table,
};

module_usb_driver(usb_streamlabs_wdt_driver);
