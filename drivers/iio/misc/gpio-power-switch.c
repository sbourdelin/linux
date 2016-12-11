/*
 * GPIO power switch driver using the industrial IO framework.
 *
 * Copyright (C) 2016 BayLibre SAS
 *
 * Author:
 *   Bartosz Golaszewski <bgolaszewski@baylibre.com.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/gpio/consumer.h>

struct gpio_pwrsw_context {
	struct gpio_desc *gpio;
};

static ssize_t gpio_pwrsw_enable_show(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct gpio_pwrsw_context *ctx = iio_priv(dev_to_iio_dev(dev));
	int val;

	val = gpiod_get_value_cansleep(ctx->gpio);
	if (val < 0)
		return val;

	return sprintf(buf, "%d\n", val);
}

static ssize_t gpio_pwrsw_enable_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t len)
{
	struct gpio_pwrsw_context *ctx = iio_priv(dev_to_iio_dev(dev));
	bool val;
	int ret;

	ret = strtobool(buf, &val);
	if (ret)
		return ret;

	gpiod_set_value_cansleep(ctx->gpio, val ? 1 : 0);

	return len;
}

static IIO_DEVICE_ATTR(in_active, 0644,
		       gpio_pwrsw_enable_show,
		       gpio_pwrsw_enable_store, 0);

static struct attribute *gpio_pwrsw_attributes[] = {
	&iio_dev_attr_in_active.dev_attr.attr,
	NULL,
};

static const struct attribute_group gpio_pwrsw_attribute_group = {
	.attrs = gpio_pwrsw_attributes,
};

static const struct iio_info gpio_pwrsw_info = {
	.driver_module = THIS_MODULE,
	.attrs = &gpio_pwrsw_attribute_group,
};

static int gpio_pwrsw_probe(struct platform_device *pdev)
{
	struct gpio_pwrsw_context *ctx;
	struct iio_dev *iio_dev;
	const char *name = NULL;
	struct device *dev;
	bool init_state;
	int gpio_flags;

	dev = &pdev->dev;

	iio_dev = devm_iio_device_alloc(dev, sizeof(*ctx));
	if (!iio_dev)
		return -ENOMEM;

	ctx = iio_priv(iio_dev);

	init_state = of_property_read_bool(dev->of_node, "power-switch-on");
	gpio_flags = init_state ? GPIOD_OUT_HIGH : GPIOD_OUT_LOW;

	ctx->gpio = devm_gpiod_get(dev, "power", gpio_flags);
	if (IS_ERR(ctx->gpio)) {
		dev_err(dev, "unable to get the power switch gpio: %ld\n",
			PTR_ERR(ctx->gpio));
		return PTR_ERR(ctx->gpio);
	}

	of_property_read_string(dev->of_node, "power-switch-name", &name);

	iio_dev->dev.parent = dev;
	iio_dev->dev.of_node = dev->of_node;
	iio_dev->name = name ? name : dev->driver->name;
	iio_dev->info = &gpio_pwrsw_info;

	return devm_iio_device_register(dev, iio_dev);
}

static const struct of_device_id gpio_pwrsw_of_match[] = {
	{ .compatible = "gpio-power-switch", },
	{ },
};

static struct platform_driver gpio_pwrsw_platform_driver = {
	.probe = gpio_pwrsw_probe,
	.driver = {
		.name = "gpio-power-switch",
		.of_match_table = gpio_pwrsw_of_match,
	},
};
module_platform_driver(gpio_pwrsw_platform_driver);

MODULE_AUTHOR("Bartosz Golaszewski <bgolaszewski@baylibre.com>");
MODULE_DESCRIPTION("GPIO power switch driver for iio");
MODULE_LICENSE("GPL v2");
