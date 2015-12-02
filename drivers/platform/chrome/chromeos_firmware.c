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

struct chromeos_firmware_gpio_info {
	int gpio;
	const char *link;
};

struct chromeos_firmware_data {
	struct chromeos_firmware_gpio_info wp;
	struct chromeos_firmware_gpio_info rec;
	struct chromeos_firmware_gpio_info dev;
};

static int dt_gpio_init(struct platform_device *pdev, const char *of_list_name,
			const char *gpio_desc_name, const char *sysfs_name,
			struct chromeos_firmware_gpio_info *gpio)
{
	int err;
	enum of_gpio_flags of_flags;
	unsigned long flags = GPIOF_DIR_IN | GPIOF_EXPORT;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *cnp;

	cnp = of_get_child_by_name(np, of_list_name);
	if (!cnp)
		/*
		 * We don't necessarily expect to find all of the devices, so
		 * return without generating an error.
		 */
		return 0;

	gpio->gpio = of_get_named_gpio_flags(cnp, "gpios", 0, &of_flags);
	if (!gpio_is_valid(gpio->gpio)) {
		err = -EINVAL;
		goto err_prop;
	}

	if (of_flags & OF_GPIO_ACTIVE_LOW)
		flags |= GPIOF_ACTIVE_LOW;

	err = gpio_request_one(gpio->gpio, flags, gpio_desc_name);
	if (err)
		goto err_prop;

	err = gpio_export_link(&pdev->dev, sysfs_name, gpio->gpio);
	if (err)
		goto err_gpio;

	gpio->link = sysfs_name;

	return 0;

err_gpio:
	gpio_free(gpio->gpio);
err_prop:
	of_node_put(cnp);

	return err;
}

static void chromeos_firmware_rem(struct device *dev,
				  struct chromeos_firmware_gpio_info *gpio)
{
	sysfs_remove_link(&dev->kobj, gpio->link);

	gpio_unexport(gpio->gpio);

	gpio_free(gpio->gpio);
}

static int chromeos_firmware_probe(struct platform_device *pdev)
{
	int err;
	struct chromeos_firmware_data *gpios;

	gpios = devm_kmalloc(&pdev->dev, sizeof(*gpios), GFP_KERNEL);
	if (!gpios)
		return -ENOMEM;

	err = dt_gpio_init(pdev, "write-protect", "firmware-write-protect",
			   "write-protect", &gpios->wp);
	if (err) {
		dev_err(&pdev->dev, "Failed to init write-protect.\n");
		goto err_wp;
	}

	err = dt_gpio_init(pdev, "recovery-switch", "firmware-recovery-switch",
			   "recovery-switch", &gpios->rec);
	if (err) {
		dev_err(&pdev->dev, "Failed to init recovery-switch.\n");
		goto err_rec;
	}

	err = dt_gpio_init(pdev, "developer-switch",
			   "firmware-developer-switch", "developer-switch",
			   &gpios->dev);
	if (err) {
		dev_err(&pdev->dev, "Failed to init developer-switch.\n");
		goto err_dev;
	}

	platform_set_drvdata(pdev, gpios);

	return 0;

err_dev:
	chromeos_firmware_rem(&pdev->dev, &gpios->rec);

err_rec:
	chromeos_firmware_rem(&pdev->dev, &gpios->wp);
err_wp:
	return err;
}

static int chromeos_firmware_remove(struct platform_device *pdev)
{
	struct chromeos_firmware_data *gpios = platform_get_drvdata(pdev);

	chromeos_firmware_rem(&pdev->dev, &gpios->dev);
	chromeos_firmware_rem(&pdev->dev, &gpios->rec);
	chromeos_firmware_rem(&pdev->dev, &gpios->wp);

	return 0;
}

static const struct of_device_id chromeos_firmware_of_match[] = {
	{ .compatible = "google,gpio-firmware" },
	{ }
};
MODULE_DEVICE_TABLE(of, chromeos_firmware_of_match);

static struct platform_driver chromeos_firmware_driver = {
	.probe = chromeos_firmware_probe,
	.remove = chromeos_firmware_remove,
	.driver = {
		.name = "chromeos_firmware",
		.of_match_table = chromeos_firmware_of_match,
	},
};
module_platform_driver(chromeos_firmware_driver);

MODULE_LICENSE("GPL");
