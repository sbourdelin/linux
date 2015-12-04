/*
 * Copyright (C) 2015 Collabora Ltd.
 *
 * based on vendor driver,
 *
 * Copyright (C) 2011 The Chromium OS Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/bcd.h>
#include <linux/gpio.h>
#include <linux/notifier.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct gpio_switch_gpio_info {
	int gpio;
	const char *link;
};

static int dt_gpio_init(struct platform_device *pdev, struct device_node *child,
			struct gpio_switch_gpio_info *gpio)
{
	int err;
	enum of_gpio_flags of_flags;
	unsigned long flags = GPIOF_DIR_IN | GPIOF_EXPORT;
	const char *name;

	err = of_property_read_string(child, "label", &name);
	if (err)
		return err;

	gpio->gpio = of_get_named_gpio_flags(child, "gpios", 0, &of_flags);
	if (!gpio_is_valid(gpio->gpio)) {
		err = -EINVAL;
		goto err_prop;
	}

	if (of_flags & OF_GPIO_ACTIVE_LOW)
		flags |= GPIOF_ACTIVE_LOW;

	if (!of_property_read_bool(child, "read-only"))
		flags |= GPIOF_EXPORT_CHANGEABLE;

	err = gpio_request_one(gpio->gpio, flags, name);
	if (err)
		goto err_prop;

	err = gpio_export_link(&pdev->dev, name, gpio->gpio);
	if (err)
		goto err_gpio;

	gpio->link = name;

	return 0;

err_gpio:
	gpio_free(gpio->gpio);
err_prop:
	of_node_put(child);

	return err;
}

static void gpio_switch_rem(struct device *dev,
			    struct gpio_switch_gpio_info *gpio)
{
	sysfs_remove_link(&dev->kobj, gpio->link);

	gpio_unexport(gpio->gpio);

	gpio_free(gpio->gpio);
}

static int gpio_switch_probe(struct platform_device *pdev)
{
	struct gpio_switch_gpio_info *gpios;
	struct device_node *child;
	struct device_node *np = pdev->dev.of_node;
	int ret;
	int i;

	i = of_get_child_count(np);
	if (i < 1)
		return i;

	gpios = devm_kmalloc(&pdev->dev, sizeof(gpios) * i, GFP_KERNEL);
	if (!gpios)
		return -ENOMEM;

	i = 0;

	for_each_child_of_node(np, child) {
		ret = dt_gpio_init(pdev, child, &gpios[i]);
		if (ret) {
			dev_err(&pdev->dev, "Failed to init child node %d.\n",
				i);
			goto err;
		}

		i++;
	}

	platform_set_drvdata(pdev, gpios);

	return 0;

err:
	while (i > 0) {
		i--;
		gpio_switch_rem(&pdev->dev, &gpios[i]);
	}

	return ret;
}

static int gpio_switch_remove(struct platform_device *pdev)
{
	struct gpio_switch_gpio_info *gpios = platform_get_drvdata(pdev);
	struct device_node *np = pdev->dev.of_node;
	int i;

	i = of_get_child_count(np);

	while (i > 0) {
		i--;
		gpio_switch_rem(&pdev->dev, &gpios[i]);
	}

	return 0;
}

static const struct of_device_id gpio_switch_of_match[] = {
	{ .compatible = "gpio-switch" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio_switch_of_match);

static struct platform_driver gpio_switch_driver = {
	.probe = gpio_switch_probe,
	.remove = gpio_switch_remove,
	.driver = {
		.name = "gpio_switch",
		.of_match_table = gpio_switch_of_match,
	},
};
module_platform_driver(gpio_switch_driver);

MODULE_LICENSE("GPL");
