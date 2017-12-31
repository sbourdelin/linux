// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for Phoenix RC Flight Controller Adapter
 *
 * Copyright (C) 2018 Marcus Folkesson <marcus.folkesson@gmail.com>
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
#include <linux/input.h>

#define PXRC_VENDOR_ID	(0x1781)
#define PXRC_PRODUCT_ID	(0x0898)

static const struct usb_device_id pxrc_table[] = {
	{ USB_DEVICE(PXRC_VENDOR_ID, PXRC_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, pxrc_table);

struct usb_pxrc {
	struct input_dev	*input_dev;
	struct usb_device	*udev;
	struct usb_interface	*interface;
	struct usb_anchor	anchor;
	__u8			epaddr;
	char			phys[64];
	unsigned char           *data;
	size_t			bsize;
	struct kref		kref;
};

#define to_pxrc_dev(d) container_of(d, struct usb_pxrc, kref)
static void pxrc_delete(struct kref *kref)
{
	struct usb_pxrc *pxrc = to_pxrc_dev(kref);

	usb_put_dev(pxrc->udev);
}

static void pxrc_usb_irq(struct urb *urb)
{
	struct usb_pxrc *pxrc = urb->context;

	switch (urb->status) {
	case 0:
		/* success */
		break;
	case -ETIME:
		/* this urb is timing out */
		dev_dbg(&pxrc->interface->dev,
			"%s - urb timed out - was the device unplugged?\n",
			__func__);
		return;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
	case -EPIPE:
		/* this urb is terminated, clean up */
		dev_dbg(&pxrc->interface->dev, "%s - urb shutting down with status: %d\n",
			__func__, urb->status);
		return;
	default:
		dev_dbg(&pxrc->interface->dev, "%s - nonzero urb status received: %d\n",
			__func__, urb->status);
		goto exit;
	}

	if (urb->actual_length == 8) {
		input_report_abs(pxrc->input_dev, ABS_X, pxrc->data[0]);
		input_report_abs(pxrc->input_dev, ABS_Y, pxrc->data[2]);
		input_report_abs(pxrc->input_dev, ABS_RX, pxrc->data[3]);
		input_report_abs(pxrc->input_dev, ABS_RY, pxrc->data[4]);
		input_report_abs(pxrc->input_dev, ABS_TILT_X, pxrc->data[5]);
		input_report_abs(pxrc->input_dev, ABS_TILT_Y, pxrc->data[6]);
		input_report_abs(pxrc->input_dev, ABS_THROTTLE, pxrc->data[7]);

		input_report_key(pxrc->input_dev, BTN_A, pxrc->data[1]);
	}

exit:
	/* Resubmit to fetch new fresh URBs */
	usb_anchor_urb(urb, &pxrc->anchor);
	if (usb_submit_urb(urb, GFP_ATOMIC) < 0)
		usb_unanchor_urb(urb);
}

static int pxrc_submit_intr_urb(struct usb_pxrc *pxrc)
{
	struct urb *urb;
	unsigned int pipe;
	int err;

	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb)
		return -ENOMEM;

	pipe = usb_rcvintpipe(pxrc->udev, pxrc->epaddr),
	usb_fill_int_urb(urb, pxrc->udev, pipe, pxrc->data, pxrc->bsize,
						pxrc_usb_irq, pxrc, 1);
	usb_anchor_urb(urb, &pxrc->anchor);
	err = usb_submit_urb(urb, GFP_KERNEL);
	if (err < 0)
		usb_unanchor_urb(urb);

	usb_free_urb(urb);
	return err;
}

static int pxrc_open(struct input_dev *input)
{
	struct usb_pxrc *pxrc = input_get_drvdata(input);
	int err;

	err = pxrc_submit_intr_urb(pxrc);
	if (err < 0)
		goto error;

	kref_get(&pxrc->kref);
	return 0;

error:
	usb_kill_anchored_urbs(&pxrc->anchor);
	return err;
}

static void pxrc_close(struct input_dev *input)
{
	struct usb_pxrc *pxrc = input_get_drvdata(input);

	usb_kill_anchored_urbs(&pxrc->anchor);
	kref_put(&pxrc->kref, pxrc_delete);
}

static int pxrc_input_init(struct usb_pxrc *pxrc)
{
	pxrc->input_dev = devm_input_allocate_device(&pxrc->interface->dev);
	if (pxrc->input_dev == NULL) {
		dev_err(&pxrc->interface->dev, "couldn't allocate input device\n");
		return -ENOMEM;
	}

	pxrc->input_dev->name = "PXRC Flight Controller Adapter";
	pxrc->input_dev->phys = pxrc->phys;
	pxrc->input_dev->id.bustype = BUS_USB;
	pxrc->input_dev->id.vendor = PXRC_VENDOR_ID;
	pxrc->input_dev->id.product = PXRC_PRODUCT_ID;
	pxrc->input_dev->id.version = 0x01;

	pxrc->input_dev->open = pxrc_open;
	pxrc->input_dev->close = pxrc_close;

	pxrc->input_dev->evbit[0] =	BIT_MASK(EV_ABS) | BIT_MASK(EV_KEY);
	pxrc->input_dev->absbit[0] =	BIT_MASK(ABS_X) |
					BIT_MASK(ABS_Y) |
					BIT_MASK(ABS_RY) |
					BIT_MASK(ABS_RX) |
					BIT_MASK(ABS_THROTTLE) |
					BIT_MASK(ABS_TILT_X) |
					BIT_MASK(ABS_TILT_Y);

	pxrc->input_dev->keybit[BIT_WORD(BTN_JOYSTICK)] = BIT_MASK(BTN_A);

	input_set_abs_params(pxrc->input_dev, ABS_X, 0, 255, 0, 0);
	input_set_abs_params(pxrc->input_dev, ABS_Y, 0, 255, 0, 0);
	input_set_abs_params(pxrc->input_dev, ABS_RX, 0, 255, 0, 0);
	input_set_abs_params(pxrc->input_dev, ABS_RY, 0, 255, 0, 0);
	input_set_abs_params(pxrc->input_dev, ABS_TILT_X, 0, 255, 0, 0);
	input_set_abs_params(pxrc->input_dev, ABS_TILT_Y, 0, 255, 0, 0);
	input_set_abs_params(pxrc->input_dev, ABS_THROTTLE, 0, 255, 0, 0);

	input_set_drvdata(pxrc->input_dev, pxrc);

	return input_register_device(pxrc->input_dev);
}

static int pxrc_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_pxrc *pxrc;
	struct usb_endpoint_descriptor *epirq;
	int retval;

	pxrc = devm_kzalloc(&interface->dev, sizeof(*pxrc), GFP_KERNEL);

	if (!pxrc)
		return -ENOMEM;

	kref_init(&pxrc->kref);
	init_usb_anchor(&pxrc->anchor);

	pxrc->udev = usb_get_dev(interface_to_usbdev(interface));
	pxrc->interface = interface;

	/* Set up the endpoint information */
	/* This device only has an interrupt endpoint */
	retval = usb_find_common_endpoints(interface->cur_altsetting,
			NULL, NULL, &epirq, NULL);
	if (retval) {
		dev_err(&interface->dev,
			"Could not find endpoint\n");
		goto error;
	}

	pxrc->bsize = usb_endpoint_maxp(epirq);
	pxrc->epaddr = epirq->bEndpointAddress;
	pxrc->data = devm_kmalloc(&interface->dev, pxrc->bsize, GFP_KERNEL);
	if (!pxrc->data) {
		retval = -ENOMEM;
		goto error;
	}

	usb_set_intfdata(interface, pxrc);
	usb_make_path(pxrc->udev, pxrc->phys, sizeof(pxrc->phys));

	retval = pxrc_input_init(pxrc);
	if (retval)
		goto error;

	return 0;

error:
	/* this frees allocated memory */
	kref_put(&pxrc->kref, pxrc_delete);

	return retval;
}

static void pxrc_disconnect(struct usb_interface *interface)
{
	struct usb_pxrc *pxrc = usb_get_intfdata(interface);

	kref_put(&pxrc->kref, pxrc_delete);
}

static struct usb_driver pxrc_driver = {
	.name =		"pxrc",
	.probe =	pxrc_probe,
	.disconnect =	pxrc_disconnect,
	.id_table =	pxrc_table,
};

module_usb_driver(pxrc_driver);

MODULE_AUTHOR("Marcus Folkesson <marcus.folkesson@gmail.com>");
MODULE_DESCRIPTION("PhoenixRC Flight Controller Adapter");
MODULE_LICENSE("GPL v2");
