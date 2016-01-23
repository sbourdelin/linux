#include <linux/module.h>
#include <linux/usb.h>

MODULE_LICENSE("GPL");

struct usb_skelswitch_product {
	const u16 idProduct;
	int (*action)(struct usb_interface *);
};

struct usb_skelswitch_vendor {
	const u16 idVendor;
	const struct usb_skelswitch_product *products;
};

static const struct usb_device_id usb_skelswitch_table[] = {
	{ USB_DEVICE(0x046d, 0xc261) },
	{ }
};

MODULE_DEVICE_TABLE(usb, usb_skelswitch_table);

static int usb_skelswitch_lg_g920(struct usb_interface *intf)
{
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_device *udev;
	int idx;
	int ret;
	int xferred;
	size_t buffer_size;
	unsigned char cmd[] = { 0x0f, 0x00, 0x01, 0x01, 0x42 };
	const size_t cmd_len = ARRAY_SIZE(cmd);
	u8 intr_out_addr = 0;

	udev = usb_get_dev(interface_to_usbdev(intf));
	iface_desc = intf->cur_altsetting;
	for (idx = 0; idx < iface_desc->desc.bNumEndpoints; idx++) {
		endpoint = &iface_desc->endpoint[idx].desc;

		if (usb_endpoint_is_int_out(endpoint)) {
			intr_out_addr = endpoint->bEndpointAddress;
			buffer_size = usb_endpoint_maxp(endpoint);
			break;
		}
	}

	if (!intr_out_addr) {
		dev_err(&udev->dev, "Logitech G920 - No interrupt out endpoint found");
		return -ENODEV;
	}

	if (buffer_size < cmd_len) {
		dev_err(&udev->dev, "usb_skelswitch: Logitech G920 - Output buffer is too small");
		return -ENODEV;
	}


	ret = usb_interrupt_msg(udev, usb_sndintpipe(udev, intr_out_addr),
				cmd, cmd_len, &xferred, USB_CTRL_SET_TIMEOUT);

	if (ret) {
		dev_err(&udev->dev, "LG G920: Failed to submit URB, errno: %d", ret);
		return ret;
	}
	if (xferred != cmd_len) {
		dev_err(&udev->dev, "LG G920: Incorrect number of bytes transferred: %d", xferred);
		return -EIO;
	}

	return 0;
}

static const struct usb_skelswitch_product usb_skelswitch_logitech_devs[] = {
	{ 0xc261, usb_skelswitch_lg_g920 },
	{ 0, NULL }
};

static const struct usb_skelswitch_vendor usb_skelswitch_vendors[] = {
	{ 0x046d, usb_skelswitch_logitech_devs },
	{ 0, NULL }
};

static int usb_skelswitch_process_products(struct usb_interface *intf, const struct usb_skelswitch_product *products, const u16 idProduct)
{
	size_t idx = 0;
	const struct usb_device *udev = interface_to_usbdev(intf);

	while (1) {
		const struct usb_skelswitch_product *product = &products[idx];

		if (product->idProduct == 0) {
			dev_err(&udev->dev, "usb_skelswitch: Unhandled idProduct 0x%04x\n", idProduct);
			return -EINVAL;
		}

		if (product->idProduct == idProduct) {
			if (product->action)
				return product->action(intf);
			else
				return 0;
		}

		idx++;
	}
}

static int usb_skelswitch_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	size_t idx = 0;
	const struct usb_device *udev = interface_to_usbdev(intf);

	while (1) {
		const struct usb_skelswitch_vendor *vendor = &usb_skelswitch_vendors[idx];

		if (vendor->idVendor == 0) {
			dev_err(&udev->dev, "Unhandled idVendor 0x%04x", id->idVendor);
			return -EINVAL;
		}

		if (id->idVendor == vendor->idVendor)
			return usb_skelswitch_process_products(intf, vendor->products, id->idProduct);

		idx++;
	}
}

static void usb_skelswitch_disconnect(struct usb_interface *intf)
{
	(void)intf;
}

static struct usb_driver usb_skelswitch_driver = {
	.disconnect = usb_skelswitch_disconnect,
	.name = "usb_skelswitch",
	.probe = usb_skelswitch_probe,
	.id_table = usb_skelswitch_table
};

module_usb_driver(usb_skelswitch_driver);

