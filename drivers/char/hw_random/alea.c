/*
 * Copyright (C) 2016 Collabora Ltd
 * Written by Bob Ham <bob.ham@collabora.com>
 *
 * An HWRNG driver to pull data from an Araneus Alea I
 *
 * derived from:
 *
 * USB Skeleton driver - 2.2
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * This driver is based on the 2.6.3 version of drivers/usb/usb-skeleton.c
 * but has been rewritten to be easier to read and use.
 *
 */

/*
 * The Alea I is a really simple device.  There is one bulk read
 * endpoint.  It spits out data in 64-byte chunks.  Each chunk
 * contains entropy.  Simple.
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/hw_random.h>


#define MODULE_NAME "alea"

#define ARANEUS_VENDOR_ID		0x12d8
#define ARANEUS_ALEA_I_PRODUCT_ID	0x0001

/* table of devices that work with this driver */
static const struct usb_device_id alea_table[] = {
	{ USB_DEVICE(ARANEUS_VENDOR_ID, ARANEUS_ALEA_I_PRODUCT_ID) },
	{ }					/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, alea_table);


/* Structure to hold all of our device specific stuff */
struct alea {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char           *bulk_in_buffer;	/* the buffer to receive data */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	int			errors;			/* the last request tanked */
	bool			ongoing_read;		/* a read is going on */
	spinlock_t		err_lock;		/* lock for errors */
	struct kref		kref;
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	wait_queue_head_t	bulk_in_wait;		/* to wait for an ongoing read */
	char			*rng_name;		/* name for the hwrng subsystem */
	struct hwrng		rng;			/* the hwrng info */
};
#define kref_to_alea(d) container_of(d, struct alea, kref)
#define rng_to_alea(d) container_of(d, struct alea, rng)

static struct usb_driver alea_driver;

static void alea_delete(struct kref *kref)
{
	struct alea *dev = kref_to_alea(kref);

	kfree(dev->rng_name);
	usb_free_urb(dev->bulk_in_urb);
	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}

static void alea_read_callback(struct urb *urb)
{
	struct alea *dev = urb->context;

	spin_lock(&dev->err_lock);
	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
		    urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero read bulk status received: %d\n",
				__func__, urb->status);

		dev->errors = urb->status;
	} else {
		dev->bulk_in_filled = urb->actual_length;
	}
	dev->ongoing_read = 0;
	spin_unlock(&dev->err_lock);

	wake_up_interruptible(&dev->bulk_in_wait);
}

static int alea_request_read(struct alea *dev)
{
	int rv;

	/* prepare a read */
	usb_fill_bulk_urb(dev->bulk_in_urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev,
				dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			dev->bulk_in_size,
			alea_read_callback,
			dev);
	/* tell everybody to leave the URB alone */
	spin_lock_irq(&dev->err_lock);
	dev->ongoing_read = 1;
	spin_unlock_irq(&dev->err_lock);

	/* submit bulk in urb, which means no data to deliver */
	dev->bulk_in_filled = 0;

	/* do it */
	rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, rv);
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irq(&dev->err_lock);
		dev->ongoing_read = 0;
		spin_unlock_irq(&dev->err_lock);
	}

	return rv;
}

static int alea_rng_read(struct hwrng *rng, void *data, size_t max, bool wait)
{
	struct alea *dev;
	int rv;
	bool ongoing_io;

	dev = rng_to_alea(rng);

	/* if we cannot read at all */
	if (!dev->bulk_in_urb)
		return 0;

	/* no concurrent readers */
	rv = mutex_lock_interruptible(&dev->io_mutex);
	if (rv < 0)
		return rv;

	if (!dev->interface) {		/* disconnect() was called */
		rv = -ENODEV;
		goto exit;
	}

	/* if IO is under way, we must not touch things */
retry:
	spin_lock_irq(&dev->err_lock);
	ongoing_io = dev->ongoing_read;
	spin_unlock_irq(&dev->err_lock);

	if (ongoing_io) {
		if (!wait) {
			rv = 0;
			goto exit;
		}

		/*
		 * IO may take forever
		 * hence wait in an interruptible state
		 */
		rv = wait_event_interruptible(dev->bulk_in_wait, (!dev->ongoing_read));
		if (rv < 0)
			goto exit;
	}

	/* errors must be reported */
	rv = dev->errors;
	if (rv < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		rv = (rv == -EPIPE) ? rv : -EIO;
		/* report it */
		goto exit;
	}

	if (dev->bulk_in_filled) {
		/* we have data to return */
		rv = min(dev->bulk_in_filled, max);
		dev->bulk_in_filled -= rv;
		memcpy(data, dev->bulk_in_buffer + dev->bulk_in_filled, rv);
	} else {
		rv = 0;
	}

	if (!dev->bulk_in_filled) {
		/* we need more data */
		int err;

		err = alea_request_read(dev);
		if (err < 0) {
			rv = err;
			goto exit;
		}

		/* possibly wait if we haven't copied any data yet */
		if (!rv && wait)
			goto retry;
	}
exit:
	mutex_unlock(&dev->io_mutex);
	return rv;
}

static int alea_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct alea *dev;
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	int i;
	int retval = -ENOMEM;
	char temp_name[1];
	int name_size;

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		dev_err(&interface->dev, "Out of memory\n");
		goto error;
	}
	kref_init(&dev->kref);
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->err_lock);
	init_waitqueue_head(&dev->bulk_in_wait);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = interface;

	dev->rng.read = alea_rng_read;

	/* set up the endpoint information */
	iface_desc = interface->cur_altsetting;
	for (i = 0; i < iface_desc->desc.bNumEndpoints; ++i) {
		endpoint = &iface_desc->endpoint[i].desc;

		if (!usb_endpoint_is_bulk_in(endpoint))
			continue;

		/* we found our bulk in endpoint */
		dev->bulk_in_size = usb_endpoint_maxp(endpoint);
		dev->bulk_in_endpointAddr = endpoint->bEndpointAddress;
		break;
	}
	if (!dev->bulk_in_endpointAddr) {
		dev_err(&interface->dev,
			"Could not find endpoint\n");
		goto error;
	}

	/* allocate objects */
	dev->bulk_in_buffer = kmalloc(dev->bulk_in_size, GFP_KERNEL);
	if (!dev->bulk_in_buffer) {
		dev_err(&interface->dev,
			"Could not allocate bulk_in_buffer\n");
		goto error;
	}
	dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!dev->bulk_in_urb) {
		dev_err(&interface->dev,
			"Could not allocate bulk_in_urb\n");
		goto error;
	}

	/* set name for hwrng */
	name_size = 1 + snprintf(temp_name, sizeof(temp_name),
				 "alea-%s", dev_name(&interface->dev));
	dev->rng_name = kmalloc(name_size, GFP_KERNEL);
	if (!dev->rng_name) {
		dev_err(&interface->dev,
			"Could not allocate rng_name\n");
		goto error;
	}
	snprintf(dev->rng_name, name_size,
		 "alea-%s", dev_name(&interface->dev));
	dev->rng.name = &dev->rng_name[0];

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	/* kick off the first read */
	retval = alea_request_read(dev);
	if (retval) {
		dev_err(&interface->dev,
			"Could not start first USB read\n");
		goto error_intf;
	}

	/* register with hwrng subsystem */
	retval = devm_hwrng_register(&dev->udev->dev, &dev->rng);
	if (retval) {
		dev_err(&interface->dev,
			"Not able to register RNG for this device.\n");
		goto error_intf;
	}

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "Araneus Alea I device now attached to RNG %s",
		 dev->rng_name);
	return 0;

error_intf:
	usb_set_intfdata(interface, NULL);
error:
	if (dev)
		/* this frees allocated memory */
		kref_put(&dev->kref, alea_delete);
	return retval;
}

static void alea_disconnect(struct usb_interface *interface)
{
	struct alea *dev;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* remove us from the hwrng subsystem */
	devm_hwrng_unregister(&dev->udev->dev, &dev->rng);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->interface = NULL;
	mutex_unlock(&dev->io_mutex);

	dev_info(&interface->dev,
		 "Araneus Alea I %s now disconnected",
		 dev->rng_name);

	/* decrement our usage count */
	kref_put(&dev->kref, alea_delete);
}

static struct usb_driver alea_driver = {
	.name =		MODULE_NAME,
	.probe =	alea_probe,
	.disconnect =	alea_disconnect,
	.id_table =	alea_table,
};

module_usb_driver(alea_driver);

MODULE_LICENSE("GPL");
