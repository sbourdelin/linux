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
	{ }
};

static const struct usb_skelswitch_vendor usb_skelswitch_vendors[] = {
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

