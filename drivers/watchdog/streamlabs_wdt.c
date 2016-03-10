/*
 * StreamLabs USB Watchdog driver
 *
 * Copyright (c) 2016 Alexey Klimov <klimov.linux@gmail.com>
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
#include <linux/usb.h>
#include <linux/watchdog.h>

/*
 * USB Watchdog device from Streamlabs
 * http://www.stream-labs.com/products/devices/watchdog/
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

/* one buffer is used for communication, however transmitted message is only
 * 32 bytes long */
#define BUFFER_TRANSFER_LENGTH	32
#define BUFFER_LENGTH		64
#define USB_TIMEOUT		350

#define STREAMLABS_CMD_START	0
#define STREAMLABS_CMD_STOP	1

#define STREAMLABS_WDT_MIN_TIMEOUT	1
#define STREAMLABS_WDT_MAX_TIMEOUT	46

struct streamlabs_wdt {
	struct watchdog_device wdt_dev;
	struct usb_device *usbdev;
	struct usb_interface *intf;

	struct kref kref;
	struct mutex lock;
	u8 *buffer;
};

static bool nowayout = WATCHDOG_NOWAYOUT;

static int usb_streamlabs_wdt_validate_response(u8 *buf)
{
	/* If watchdog device understood the command it will acknowledge
	 * with values 1,2,3,4 at indexes 10, 11, 12, 13 in response message.
	 */
	if (buf[10] != 1 || buf[11] != 2 || buf[12] != 3 || buf[13] != 4)
		return -EINVAL;

	return 0;
}

static int usb_streamlabs_wdt_command(struct watchdog_device *wdt_dev, int cmd)
{
	struct streamlabs_wdt *streamlabs_wdt = watchdog_get_drvdata(wdt_dev);
	int retval;
	int size;
	unsigned long timeout_msec;
	int retry_counter = 10;		/* how many times to re-send stop cmd */

	mutex_lock(&streamlabs_wdt->lock);

	timeout_msec = wdt_dev->timeout * MSEC_PER_SEC;

	/* Prepare message that will be sent to device.
	 * This buffer is allocated by kzalloc(). Only initialize required
	 * fields.
	 */
	if (cmd == STREAMLABS_CMD_START) {
		streamlabs_wdt->buffer[0] = 0xcc;
		streamlabs_wdt->buffer[1] = 0xaa;
	} else {	/* assume stop command if it's not start */
		streamlabs_wdt->buffer[0] = 0xff;
		streamlabs_wdt->buffer[1] = 0xbb;
	}

	streamlabs_wdt->buffer[3] = 0x80;

	streamlabs_wdt->buffer[6] = (timeout_msec & 0xff) << 8;
	streamlabs_wdt->buffer[7] = (timeout_msec & 0xff00) >> 8;
retry:
	streamlabs_wdt->buffer[10] = 0x00;
	streamlabs_wdt->buffer[11] = 0x00;
	streamlabs_wdt->buffer[12] = 0x00;
	streamlabs_wdt->buffer[13] = 0x00;

	/* send command to watchdog */
	retval = usb_interrupt_msg(streamlabs_wdt->usbdev,
				usb_sndintpipe(streamlabs_wdt->usbdev, 0x02),
				streamlabs_wdt->buffer, BUFFER_TRANSFER_LENGTH,
				&size, USB_TIMEOUT);

	if (retval || size != BUFFER_TRANSFER_LENGTH) {
		dev_err(&streamlabs_wdt->intf->dev,
			"error %i when submitting interrupt msg\n", retval);
		retval = -EIO;
		goto out;
	}

	/* and read response from watchdog */
	retval = usb_interrupt_msg(streamlabs_wdt->usbdev,
				usb_rcvintpipe(streamlabs_wdt->usbdev, 0x81),
				streamlabs_wdt->buffer, BUFFER_LENGTH,
				&size, USB_TIMEOUT);

	if (retval || size != BUFFER_LENGTH) {
		dev_err(&streamlabs_wdt->intf->dev,
			"error %i when receiving interrupt msg\n", retval);
		retval = -EIO;
		goto out;
	}

	/* check if watchdog actually acked/recognized command */
	retval = usb_streamlabs_wdt_validate_response(streamlabs_wdt->buffer);
	if (retval) {
		dev_err(&streamlabs_wdt->intf->dev,
					"watchdog didn't ACK command!\n");
		goto out;
	}

	/* Transition from enabled to disabled state in this device
	 * doesn't happen immediately. Usually, 2 or 3 (sometimes even 4) stop
	 * commands should be sent until watchdog answers 'I'm stopped!'.
	 * Retry stop command if watchdog fails to answer correctly
	 * about its state. After 10 attempts, report error and return -EIO.
	 */
	if (cmd == STREAMLABS_CMD_STOP) {
		if (--retry_counter <= 0) {
			dev_err(&streamlabs_wdt->intf->dev,
				"failed to stop watchdog after 9 attempts!\n");
			retval = -EIO;
			goto out;
		}
		/*
		 * Check if watchdog actually changed state to disabled.
		 * If watchdog is still enabled then reset message and retry
		 * stop command.
		 */
		if (streamlabs_wdt->buffer[0] != 0xff ||
					streamlabs_wdt->buffer[1] != 0xbb) {
			streamlabs_wdt->buffer[0] = 0xff;
			streamlabs_wdt->buffer[1] = 0xbb;
			goto retry;
		}
	}

out:
	mutex_unlock(&streamlabs_wdt->lock);
	return retval;
}

static int usb_streamlabs_wdt_start(struct watchdog_device *wdt_dev)
{
	return usb_streamlabs_wdt_command(wdt_dev, STREAMLABS_CMD_START);
}

static int usb_streamlabs_wdt_stop(struct watchdog_device *wdt_dev)
{
	return usb_streamlabs_wdt_command(wdt_dev, STREAMLABS_CMD_STOP);
}

static int usb_streamlabs_wdt_settimeout(struct watchdog_device *wdt_dev,
				unsigned int timeout)
{
	struct streamlabs_wdt *streamlabs_wdt = watchdog_get_drvdata(wdt_dev);

	mutex_lock(&streamlabs_wdt->lock);
	wdt_dev->timeout = timeout;
	mutex_unlock(&streamlabs_wdt->lock);

	return 0;
}

static void usb_streamlabs_wdt_release_resources(struct kref *kref)
{
	struct streamlabs_wdt *streamlabs_wdt =
			container_of(kref, struct streamlabs_wdt, kref);

	mutex_destroy(&streamlabs_wdt->lock);
	kfree(streamlabs_wdt->buffer);
	kfree(streamlabs_wdt);
}

static void usb_streamlabs_wdt_ref(struct watchdog_device *wdt_dev)
{
	struct streamlabs_wdt *streamlabs_wdt = watchdog_get_drvdata(wdt_dev);

	kref_get(&streamlabs_wdt->kref);
}

static void usb_streamlabs_wdt_unref(struct watchdog_device *wdt_dev)
{
	struct streamlabs_wdt *streamlabs_wdt = watchdog_get_drvdata(wdt_dev);

	kref_put(&streamlabs_wdt->kref, usb_streamlabs_wdt_release_resources);
}

static const struct watchdog_info streamlabs_wdt_ident = {
	.options	= (WDIOF_SETTIMEOUT | WDIOF_KEEPALIVEPING),
	.identity	= DRIVER_NAME,
};

static struct watchdog_ops usb_streamlabs_wdt_ops = {
	.owner	= THIS_MODULE,
	.start	= usb_streamlabs_wdt_start,
	.stop	= usb_streamlabs_wdt_stop,
	.set_timeout	= usb_streamlabs_wdt_settimeout,
	.ref	= usb_streamlabs_wdt_ref,
	.unref	= usb_streamlabs_wdt_unref,
};

static int usb_streamlabs_wdt_probe(struct usb_interface *intf,
			const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct streamlabs_wdt *streamlabs_wdt;
	int retval;

	/* USB IDs of this device appear to be weird/unregistered. Hence, do
	 * an additional check on product and manufacturer.
	 * If there is similar device in the field with same values then
	 * there is stop command in probe() below that checks if the device
	 * behaves as a watchdog. */
	if (dev->product && dev->manufacturer &&
		(strncmp(dev->product, "USBkit", 6) != 0
		|| strncmp(dev->manufacturer, "STREAM LABS", 11) != 0))
		return -ENODEV;

	streamlabs_wdt = kmalloc(sizeof(struct streamlabs_wdt), GFP_KERNEL);
	if (!streamlabs_wdt) {
		dev_err(&intf->dev, "kmalloc failed\n");
		return -ENOMEM;
	}

	streamlabs_wdt->buffer = kzalloc(BUFFER_LENGTH, GFP_KERNEL);
	if (!streamlabs_wdt->buffer) {
		dev_err(&intf->dev, "kzalloc for watchdog->buffer failed\n");
		retval = -ENOMEM;
		goto err_nobuf;
	}

	mutex_init(&streamlabs_wdt->lock);

	streamlabs_wdt->wdt_dev.info = &streamlabs_wdt_ident;
	streamlabs_wdt->wdt_dev.ops = &usb_streamlabs_wdt_ops;
	streamlabs_wdt->wdt_dev.timeout = STREAMLABS_WDT_MAX_TIMEOUT;
	streamlabs_wdt->wdt_dev.max_timeout = STREAMLABS_WDT_MAX_TIMEOUT;
	streamlabs_wdt->wdt_dev.min_timeout = STREAMLABS_WDT_MIN_TIMEOUT;
	streamlabs_wdt->wdt_dev.parent = &intf->dev;

	streamlabs_wdt->usbdev = interface_to_usbdev(intf);
	streamlabs_wdt->intf = intf;
	usb_set_intfdata(intf, &streamlabs_wdt->wdt_dev);
	watchdog_set_drvdata(&streamlabs_wdt->wdt_dev, streamlabs_wdt);

	watchdog_init_timeout(&streamlabs_wdt->wdt_dev,
				streamlabs_wdt->wdt_dev.timeout, &intf->dev);
	watchdog_set_nowayout(&streamlabs_wdt->wdt_dev, nowayout);

	kref_init(&streamlabs_wdt->kref);

	retval = usb_streamlabs_wdt_stop(&streamlabs_wdt->wdt_dev);
	if (retval)
		goto err_wdt_buf;

	retval = watchdog_register_device(&streamlabs_wdt->wdt_dev);
	if (retval) {
		dev_err(&intf->dev, "failed to register watchdog device\n");
		goto err_wdt_buf;
	}

	dev_info(&intf->dev, "StreamLabs USB watchdog loaded.\n");

	return 0;

err_wdt_buf:
	mutex_destroy(&streamlabs_wdt->lock);
	kfree(streamlabs_wdt->buffer);

err_nobuf:
	kfree(streamlabs_wdt);
	return retval;
}



static int usb_streamlabs_wdt_suspend(struct usb_interface *intf,
					pm_message_t message)
{
	struct streamlabs_wdt *streamlabs_wdt = usb_get_intfdata(intf);

	if (watchdog_active(&streamlabs_wdt->wdt_dev))
		usb_streamlabs_wdt_command(&streamlabs_wdt->wdt_dev,
							STREAMLABS_CMD_STOP);

	return 0;
}

static int usb_streamlabs_wdt_resume(struct usb_interface *intf)
{
	struct streamlabs_wdt *streamlabs_wdt = usb_get_intfdata(intf);

	if (watchdog_active(&streamlabs_wdt->wdt_dev))
		return usb_streamlabs_wdt_command(&streamlabs_wdt->wdt_dev,
							STREAMLABS_CMD_START);

	return 0;
}

static void usb_streamlabs_wdt_disconnect(struct usb_interface *intf)
{
	struct streamlabs_wdt *streamlabs_wdt = usb_get_intfdata(intf);

	/* First, stop sending USB messages to device. */
	mutex_lock(&streamlabs_wdt->lock);
	usb_set_intfdata(intf, NULL);
	streamlabs_wdt->usbdev = NULL;
	mutex_unlock(&streamlabs_wdt->lock);

	/* after commincation with device has stopped we can
	 * unregister watchdog device. unref callback will clear the rest on
	 * release of device if it was opened.
	 */
	watchdog_unregister_device(&streamlabs_wdt->wdt_dev);
	kref_put(&streamlabs_wdt->kref, usb_streamlabs_wdt_release_resources);
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
