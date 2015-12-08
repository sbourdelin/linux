#ifndef __LINUX_USB_GENERIC_HUB_H
#define __LINUX_USB_GENERIC_HUB_H

struct usb_hub_generic_platform_data {
	int gpio_reset;
	int gpio_reset_polarity;
	int gpio_reset_duration_us;
	struct clk *ext_clk;
};

#endif /* __LINUX_USB_GENERIC_HUB_H */
