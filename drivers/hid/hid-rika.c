/*
 * Riso Kagaku Webmail Notifier USB RGB LED driver
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

#define REPORT_SIZE	6

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

struct rika_led {
	struct led_classdev	cdev;
	struct rika_device	*rdev;
	char			name[32];
};

struct rika_device {
	struct rika_led		red;
	struct rika_led		green;
	struct rika_led		blue;
	struct hid_device       *hdev;
	struct mutex		lock;
};

#define to_rika_led(arg) container_of(arg, struct rika_led, cdev)

static bool switch_green_blue;
module_param(switch_green_blue, bool, 0);
MODULE_PARM_DESC(switch_green_blue, "switch green and blue RGB component");

static u8 rika_index(struct rika_device *rdev)
{
	enum led_brightness r, g, b;

	r = rdev->red.cdev.brightness;
	g = rdev->green.cdev.brightness;
	b = rdev->blue.cdev.brightness;

	if (switch_green_blue)
		return RISO_KAGAKU_IX(r, b, g);
	else
		return RISO_KAGAKU_IX(r, g, b);
}

static int rika_write_color(struct led_classdev *cdev, enum led_brightness br)
{
	struct rika_led *rled = to_rika_led(cdev);
	struct rika_device *rdev = rled->rdev;
	__u8 buf[REPORT_SIZE] = {};
	int ret;

	buf[1] = rika_index(rdev);

	mutex_lock(&rdev->lock);
	ret = hid_hw_output_report(rdev->hdev, buf, REPORT_SIZE);
	mutex_unlock(&rdev->lock);

	if (ret < 0)
		return ret;

	return (ret == REPORT_SIZE) ? 0 : -EMSGSIZE;
}

static int rika_init_led(struct rika_led *led, const char *color_name,
			 struct rika_device *rdev, int minor)
{
	snprintf(led->name, sizeof(led->name), "rika%d:%s", minor, color_name);
	led->cdev.name = led->name;
	led->cdev.max_brightness = 1;
	led->cdev.brightness_set_blocking = rika_write_color;
	led->cdev.flags = LED_HW_PLUGGABLE;
	led->rdev = rdev;

	return devm_led_classdev_register(&rdev->hdev->dev, &led->cdev);
}

static int rika_init_rgb(struct rika_device *rdev, int minor)
{
	int ret;

	/* Register the red diode */
	ret = rika_init_led(&rdev->red, "red", rdev, minor);
	if (ret)
		return ret;

	/* Register the green diode */
	ret = rika_init_led(&rdev->green, "green", rdev, minor);
	if (ret)
		return ret;

	/* Register the blue diode */
	return rika_init_led(&rdev->blue, "blue", rdev, minor);
}

static int rika_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct rika_device *rdev;
	int ret, minor;

	rdev = devm_kzalloc(&hdev->dev, sizeof(*rdev), GFP_KERNEL);
	if (!rdev)
		return -ENOMEM;

	ret = hid_parse(hdev);
	if (ret)
		return ret;

	rdev->hdev = hdev;
	mutex_init(&rdev->lock);

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret)
		return ret;

	minor = ((struct hidraw *) hdev->hidraw)->minor;

	ret = rika_init_rgb(rdev, minor);
	if (ret) {
		hid_hw_stop(hdev);
		return ret;
	}

	dev_info(&hdev->dev, "RiKa Webmail Notifier %d initialized\n", minor);

	return 0;
}

static const struct hid_device_id rika_table[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_RISO_KAGAKU,
	  USB_DEVICE_ID_RI_KA_WEBMAIL) },
	{ }
};
MODULE_DEVICE_TABLE(hid, rika_table);

static struct hid_driver rika_driver = {
	.name = "rika",
	.probe = rika_probe,
	.id_table = rika_table,
};

module_hid_driver(rika_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Heiner Kallweit <hkallweit1@gmail.com>");
MODULE_DESCRIPTION("Riso Kagaku Webmail Notifier USB RGB LED driver");
