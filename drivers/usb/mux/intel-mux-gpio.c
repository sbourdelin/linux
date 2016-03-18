/*
 * USB Dual Role Port Mux driver controlled by gpios
 *
 * Copyright (c) 2016, Intel Corporation.
 * Author: David Cohen <david.a.cohen@linux.intel.com>
 * Author: Lu Baolu <baolu.lu@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/acpi.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/usb/intel-mux.h>

struct vuport {
	struct device *dev;
	struct gpio_desc *gpio_vbus_en;
	struct gpio_desc *gpio_usb_mux;
};

static struct vuport *vup;

#define VUPORT_EXTCON_DEV_NAME	"extcon-usb-gpio.0"

/*
 * id == 0, HOST connected, USB port should be set to peripheral
 * id == 1, HOST disconnected, USB port should be set to host
 *
 * Peripheral: set USB mux to peripheral and disable VBUS
 * Host: set USB mux to host and enable VBUS
 */
static inline int vuport_set_port(struct device *dev, int id)
{
	dev_dbg(dev, "USB PORT ID: %s\n", id ? "HOST" : "PERIPHERAL");

	gpiod_set_value_cansleep(vup->gpio_usb_mux, !id);
	gpiod_set_value_cansleep(vup->gpio_vbus_en, id);

	return 0;
}

static int vuport_cable_set(struct device *dev)
{
	return vuport_set_port(dev, 1);
}

static int vuport_cable_unset(struct device *dev)
{
	return vuport_set_port(dev, 0);
}

static int vuport_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;

	vup = devm_kzalloc(dev, sizeof(*vup), GFP_KERNEL);
	if (!vup)
		return -ENOMEM;

	/* retrieve vbus and mux gpios */
	vup->gpio_vbus_en = devm_gpiod_get_optional(dev,
				"vbus_en", GPIOD_ASIS);
	if (IS_ERR(vup->gpio_vbus_en))
		return PTR_ERR(vup->gpio_vbus_en);

	vup->gpio_usb_mux = devm_gpiod_get_optional(dev,
				"usb_mux", GPIOD_ASIS);
	if (IS_ERR(vup->gpio_usb_mux))
		return PTR_ERR(vup->gpio_usb_mux);

	vup->dev = dev;

	return intel_usb_mux_bind_cable(dev, VUPORT_EXTCON_DEV_NAME,
					vuport_cable_set,
					vuport_cable_unset);
}

static int vuport_remove(struct platform_device *pdev)
{
	return intel_usb_mux_unbind_cable(&pdev->dev);
}

#ifdef CONFIG_PM_SLEEP
/*
 * In case a micro A cable was plugged in while device was sleeping,
 * we missed the interrupt. We need to poll usb id gpio when waking the
 * driver to detect the missed event.
 * We use 'complete' callback to give time to all extcon listeners to
 * resume before we send new events.
 */
static const struct dev_pm_ops vuport_pm_ops = {
	.complete = intel_usb_mux_complete,
};
#endif

static const struct platform_device_id vuport_platform_ids[] = {
	{ .name = "intel-mux-gpio", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(platform, vuport_platform_ids);

static struct platform_driver vuport_driver = {
	.driver = {
		.name = "intel-mux-gpio",
#ifdef CONFIG_PM_SLEEP
		.pm = &vuport_pm_ops,
#endif
	},
	.probe = vuport_probe,
	.remove = vuport_remove,
	.id_table = vuport_platform_ids,
};

module_platform_driver(vuport_driver);

MODULE_AUTHOR("David Cohen <david.a.cohen@linux.intel.com>");
MODULE_AUTHOR("Lu Baolu <baolu.lu@linux.intel.com>");
MODULE_DESCRIPTION("Intel USB gpio mux driver");
MODULE_LICENSE("GPL v2");
