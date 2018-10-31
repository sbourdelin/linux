// SPDX-License-Identifier: GPL-2.0
//
// UP Board LED driver
//
// Copyright (c) 2018, Emutex Ltd.
//
// Author: Javier Arteaga <javier@emutex.com>
//

#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/mfd/upboard.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#define to_upboard_led(cdev) container_of(cdev, struct upboard_led, cdev)

static const char * const upboard_led_names[] = {
	"upboard:blue:",
	"upboard:yellow:",
	"upboard:green:",
	"upboard:red:",
};

struct upboard_led {
	struct regmap_field *field;
	struct led_classdev cdev;
};

static enum led_brightness upboard_led_brightness_get(struct led_classdev *cdev)
{
	struct upboard_led *led = to_upboard_led(cdev);
	int ret, brightness = 0;

	ret = regmap_field_read(led->field, &brightness);
	if (ret < 0)
		dev_err(cdev->dev, "Failed to get led brightness, %d", ret);

	return brightness;
}

static void upboard_led_brightness_set(struct led_classdev *cdev,
				       enum led_brightness brightness)
{
	struct upboard_led *led = to_upboard_led(cdev);
	int ret;

	ret = regmap_field_write(led->field, brightness);
	if (ret < 0)
		dev_err(cdev->dev, "Failed to set led brightness, %d", ret);
}

static int upboard_led_probe(struct platform_device *pdev)
{
	unsigned int led_index = pdev->id;
	struct device *dev = &pdev->dev;
	struct upboard_led *led;
	struct regmap *regmap;
	struct reg_field conf = {
		.reg = UPBOARD_REG_FUNC_EN0,
		.lsb = led_index,
		.msb = led_index,
	};

	if (led_index >= ARRAY_SIZE(upboard_led_names))
		return -EINVAL;

	if (!dev->parent)
		return -EINVAL;

	regmap = dev_get_regmap(dev->parent, NULL);
	if (!regmap)
		return -EINVAL;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	led->field = devm_regmap_field_alloc(dev, regmap, conf);
	if (IS_ERR(led->field))
		return PTR_ERR(led->field);

	led->cdev.max_brightness = 1;
	led->cdev.brightness_get = upboard_led_brightness_get;
	led->cdev.brightness_set = upboard_led_brightness_set;
	led->cdev.name = upboard_led_names[led_index];

	return devm_led_classdev_register(dev, &led->cdev);
}

static struct platform_driver upboard_led_driver = {
	.driver = {
		.name = "upboard-led",
	},
};

module_platform_driver_probe(upboard_led_driver, upboard_led_probe);

MODULE_ALIAS("platform:upboard-led");
MODULE_AUTHOR("Javier Arteaga <javier@emutex.com>");
MODULE_DESCRIPTION("UP Board LED driver");
MODULE_LICENSE("GPL v2");
