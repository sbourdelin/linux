/*
 * Simple USB RGB LED driver
 *
 * Copyright 2016 Heiner Kallweit <hkallweit1@gmail.com>
 * Based on drivers/hid/hid-thingm.c and
 * drivers/usb/misc/usbled.c
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2.
 */

#include <linux/hid.h>
#include <linux/hidraw.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/mutex.h>

#include "hid-ids.h"

enum led_report_type {
	RAW_REQUEST,
	OUTPUT_REPORT
};

static unsigned const char riso_kagaku_tbl[] = {
/* R+2G+4B -> riso kagaku color index */
	[0] = 0, /* black   */
	[1] = 2, /* red     */
	[2] = 1, /* green   */
	[3] = 5, /* yellow  */
	[4] = 3, /* blue    */
	[5] = 6, /* magenta */
	[6] = 4, /* cyan    */
	[7] = 7  /* white   */
};

#define RISO_KAGAKU_IX(r, g, b) riso_kagaku_tbl[((r)?1:0)+((g)?2:0)+((b)?4:0)]

struct usbled_device;

struct usbled_type {
	const char		*name;
	const char		*short_name;
	enum led_brightness	max_brightness;
	size_t			report_size;
	enum led_report_type	report_type;
	u8			report_id;
	int (*init)(struct usbled_device *udev);
	int (*write)(struct led_classdev *cdev, enum led_brightness br);
};

struct usbled_led {
	struct led_classdev	cdev;
	struct usbled_device	*udev;
	char			name[32];
};

struct usbled_device {
	const struct usbled_type *type;
	struct usbled_led	red;
	struct usbled_led	green;
	struct usbled_led	blue;
	struct hid_device       *hdev;
	struct mutex		lock;
};

#define MAX_REPORT_SIZE		16

#define to_usbled_led(arg) container_of(arg, struct usbled_led, cdev)

static bool riso_kagaku_switch_green_blue;
module_param(riso_kagaku_switch_green_blue, bool, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(riso_kagaku_switch_green_blue,
	"switch green and blue RGB component for Riso Kagaku devices");

static int usbled_send(struct usbled_device *udev, __u8 *buf)
{
	int ret;

	buf[0] = udev->type->report_id;

	mutex_lock(&udev->lock);

	if (udev->type->report_type == RAW_REQUEST)
		ret = hid_hw_raw_request(udev->hdev, buf[0], buf,
					 udev->type->report_size,
					 HID_FEATURE_REPORT,
					 HID_REQ_SET_REPORT);
	else if (udev->type->report_type == OUTPUT_REPORT)
		ret = hid_hw_output_report(udev->hdev, buf,
					   udev->type->report_size);
	else
		ret = -EINVAL;

	mutex_unlock(&udev->lock);

	if (ret < 0)
		return ret;

	return ret == udev->type->report_size ? 0 : -EMSGSIZE;
}

static u8 riso_kagaku_index(struct usbled_device *udev)
{
	enum led_brightness r, g, b;

	r = udev->red.cdev.brightness;
	g = udev->green.cdev.brightness;
	b = udev->blue.cdev.brightness;

	if (riso_kagaku_switch_green_blue)
		return RISO_KAGAKU_IX(r, b, g);
	else
		return RISO_KAGAKU_IX(r, g, b);
}

static int riso_kagaku_write(struct led_classdev *cdev, enum led_brightness br)
{
	struct usbled_led *uled = to_usbled_led(cdev);
	struct usbled_device *udev = uled->udev;
	__u8 buf[MAX_REPORT_SIZE] = {};

	buf[1] = riso_kagaku_index(udev);

	return usbled_send(udev, buf);
}

static const struct usbled_type usbled_riso_kagaku = {
	.name = "Riso Kagaku Webmail Notifier",
	.short_name = "riso_kagaku",
	.max_brightness = 1,
	.report_size = 6,
	.report_type = OUTPUT_REPORT,
	.report_id = 0,
	.write = riso_kagaku_write,
};

static int dream_cheeky_write(struct led_classdev *cdev, enum led_brightness br)
{
	struct usbled_led *uled = to_usbled_led(cdev);
	struct usbled_device *udev = uled->udev;
	__u8 buf[MAX_REPORT_SIZE] = {};

	buf[1] = udev->red.cdev.brightness;
	buf[2] = udev->green.cdev.brightness;
	buf[3] = udev->blue.cdev.brightness;
	buf[7] = 0x1a;
	buf[8] = 0x05;

	return usbled_send(udev, buf);
}

static int dream_cheeky_init(struct usbled_device *udev)
{
	__u8 buf[MAX_REPORT_SIZE] = {};

	/* Dream Cheeky magic */
	buf[1] = 0x1f;
	buf[2] = 0x02;
	buf[4] = 0x5f;
	buf[7] = 0x1a;
	buf[8] = 0x03;

	return usbled_send(udev, buf);
}

static const struct usbled_type usbled_dream_cheeky = {
	.name = "Dream Cheeky Webmail Notifier",
	.short_name = "dream_cheeky",
	.max_brightness = 31,
	.report_size = 9,
	.report_type = RAW_REQUEST,
	.report_id = 0,
	.init = dream_cheeky_init,
	.write = dream_cheeky_write,
};

static int usbled_init_led(struct usbled_led *led, const char *color_name,
			   struct usbled_device *udev, unsigned int minor)
{
	snprintf(led->name, sizeof(led->name), "%s%u:%s",
		 udev->type->short_name, minor, color_name);
	led->cdev.name = led->name;
	led->cdev.max_brightness = udev->type->max_brightness;
	led->cdev.brightness_set_blocking = udev->type->write;
	led->cdev.flags = LED_HW_PLUGGABLE;
	led->udev = udev;

	return devm_led_classdev_register(&udev->hdev->dev, &led->cdev);
}

static int usbled_init_rgb(struct usbled_device *udev, unsigned int minor)
{
	int ret;

	/* Register the red diode */
	ret = usbled_init_led(&udev->red, "red", udev, minor);
	if (ret)
		return ret;

	/* Register the green diode */
	ret = usbled_init_led(&udev->green, "green", udev, minor);
	if (ret)
		return ret;

	/* Register the blue diode */
	return usbled_init_led(&udev->blue, "blue", udev, minor);
}

static int usbled_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct usbled_device *udev;
	unsigned int minor;
	int ret;

	udev = devm_kzalloc(&hdev->dev, sizeof(*udev), GFP_KERNEL);
	if (!udev)
		return -ENOMEM;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	udev->hdev = hdev;
	udev->type = (const struct usbled_type *)id->driver_data;
	mutex_init(&udev->lock);

	if (udev->type->init) {
		ret = udev->type->init(udev);
		if (ret)
			return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	minor = ((struct hidraw *) hdev->hidraw)->minor;

	ret = usbled_init_rgb(udev, minor);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	dev_info(&hdev->dev, "%s initialized\n", udev->type->name);

	return 0;
}

static const struct hid_device_id usbled_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_RISO_KAGAKU,
	  USB_DEVICE_ID_RI_KA_WEBMAIL),
	  .driver_data = (kernel_ulong_t)&usbled_riso_kagaku },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DREAM_CHEEKY,
	  USB_DEVICE_ID_DREAM_CHEEKY_WN),
	  .driver_data = (kernel_ulong_t)&usbled_dream_cheeky },
	{ HID_USB_DEVICE(USB_VENDOR_ID_DREAM_CHEEKY,
	  USB_DEVICE_ID_DREAM_CHEEKY_FA),
	  .driver_data = (kernel_ulong_t)&usbled_dream_cheeky },
	{ }
};
MODULE_DEVICE_TABLE(hid, usbled_table);

static struct hid_driver usbled_driver = {
	.name = "usb-led",
	.probe = usbled_probe,
	.id_table = usbled_table,
};

module_hid_driver(usbled_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heiner Kallweit <hkallweit1@gmail.com>");
MODULE_DESCRIPTION("Simple USB RGB LED driver");
