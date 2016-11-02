/*
 * Advantech iManager GPIO driver
 *
 * Copyright (C) 2016 Advantech Co., Ltd.
 * Author: Richard Vidal-Dorsch <richard.dorsch@advantech.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/mfd/imanager.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#define EC_GPIOF_DIR_OUT	BIT(6)
#define EC_GPIOF_DIR_IN		BIT(7)

struct imanager_gpio_data {
	struct imanager_device_data *imgr;
	struct gpio_chip chip;
};

static int imanager_gpio_direction_in(struct gpio_chip *chip, uint offset)
{
	struct imanager_gpio_data *data = gpiochip_get_data(chip);
	struct imanager_device_data *imgr = data->imgr;
	struct imanager_device_attribute *attr = imgr->ec.gpio.attr[offset];

	mutex_lock(&imgr->lock);
	imanager_write8(&imgr->ec, EC_CMD_GPIO_DIR_WR, attr->did,
			EC_GPIOF_DIR_IN);
	mutex_unlock(&imgr->lock);

	return 0;
}

static int
imanager_gpio_direction_out(struct gpio_chip *chip, uint offset, int val)
{
	struct imanager_gpio_data *data = gpiochip_get_data(chip);
	struct imanager_device_data *imgr = data->imgr;
	struct imanager_device_attribute *attr = imgr->ec.gpio.attr[offset];

	mutex_lock(&imgr->lock);
	imanager_write8(&imgr->ec, EC_CMD_GPIO_DIR_WR, attr->did,
			EC_GPIOF_DIR_OUT);
	mutex_unlock(&imgr->lock);

	return 0;
}

static int imanager_gpio_get_direction(struct gpio_chip *chip, uint offset)
{
	struct imanager_gpio_data *data = gpiochip_get_data(chip);
	struct imanager_device_data *imgr = data->imgr;
	struct imanager_device_attribute *attr = imgr->ec.gpio.attr[offset];
	int ret;

	mutex_lock(&imgr->lock);
	ret = imanager_read8(&imgr->ec, EC_CMD_GPIO_DIR_RD, attr->did);
	mutex_unlock(&imgr->lock);

	return ret & EC_GPIOF_DIR_IN ? GPIOF_DIR_IN : GPIOF_DIR_OUT;
}

static int imanager_gpio_get(struct gpio_chip *chip, uint offset)
{
	struct imanager_gpio_data *data = gpiochip_get_data(chip);
	struct imanager_device_data *imgr = data->imgr;
	struct imanager_device_attribute *attr = imgr->ec.gpio.attr[offset];
	int ret;

	mutex_lock(&imgr->lock);
	ret = imanager_read8(&imgr->ec, EC_CMD_HWP_RD, attr->did);
	mutex_unlock(&imgr->lock);

	return ret;
}

static void imanager_gpio_set(struct gpio_chip *chip, uint offset, int val)
{
	struct imanager_gpio_data *data = gpiochip_get_data(chip);
	struct imanager_device_data *imgr = data->imgr;
	struct imanager_device_attribute *attr = imgr->ec.gpio.attr[offset];

	mutex_lock(&imgr->lock);
	imanager_write8(&imgr->ec, EC_CMD_HWP_WR, attr->did, val);
	mutex_unlock(&imgr->lock);
}

static int imanager_gpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct imanager_device_data *imgr = dev_get_drvdata(dev->parent);
	struct imanager_gpio_data *gpio;
	struct gpio_chip *chip;
	int ret;

	gpio = devm_kzalloc(dev, sizeof(*gpio), GFP_KERNEL);
	if (!gpio)
		return -ENOMEM;

	gpio->imgr = imgr;

	platform_set_drvdata(pdev, gpio);

	chip = &gpio->chip;

	chip->owner = THIS_MODULE;
	chip->parent = dev;
	chip->label = "gpio-imanager";
	chip->base = -1;
	chip->ngpio = imgr->ec.gpio.num;
	chip->get = imanager_gpio_get;
	chip->set = imanager_gpio_set;
	chip->direction_input = imanager_gpio_direction_in;
	chip->direction_output = imanager_gpio_direction_out;
	chip->get_direction = imanager_gpio_get_direction;
	if (!chip->ngpio) {
		dev_err(dev, "No GPIO pins detected\n");
		return -ENODEV;
	}

	ret = devm_gpiochip_add_data(dev, chip, gpio);
	if (ret < 0) {
		dev_err(dev, "Could not register GPIO chip\n");
		return ret;
	}

	dev_info(dev, "GPIO initialized with %d pins\n", chip->ngpio);

	return 0;
}

static struct platform_driver imanager_gpio_driver = {
	.driver = {
		.name	= "imanager-gpio",
	},
	.probe	= imanager_gpio_probe,
};

module_platform_driver(imanager_gpio_driver);

MODULE_DESCRIPTION("Advantech iManager GPIO Driver");
MODULE_AUTHOR("Richard Vidal-Dorsch <richard.dorsch at advantech.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:imanager-gpio");
