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
	struct intel_mux_dev umdev;
	struct gpio_desc *gpio_vbus_en;
	struct gpio_desc *gpio_usb_mux;
};

/*
 * id == 0, HOST connected, USB port should be set to peripheral
 * id == 1, HOST disconnected, USB port should be set to host
 *
 * Peripheral: set USB mux to peripheral and disable VBUS
 * Host: set USB mux to host and enable VBUS
 */
static inline int vuport_set_port(struct intel_mux_dev *umdev, int id)
{
	struct vuport *vup = container_of(umdev, struct vuport, umdev);

	dev_dbg(umdev->dev, "USB PORT ID: %s\n", id ? "HOST" : "PERIPHERAL");

	gpiod_set_value_cansleep(vup->gpio_usb_mux, !id);
	gpiod_set_value_cansleep(vup->gpio_vbus_en, id);

	return 0;
}

static int vuport_cable_set(struct intel_mux_dev *umdev)
{
	return vuport_set_port(umdev, 1);
}

static int vuport_cable_unset(struct intel_mux_dev *umdev)
{
	return vuport_set_port(umdev, 0);
}

static int vuport_probe(struct platform_device *pdev)
{
	struct intel_mux_dev *umdev;
	struct device *dev = &pdev->dev;
	struct vuport *vup;

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

	/* populate the mux generic structure */
	umdev = &vup->umdev;
	umdev->dev = dev;
	umdev->cable_name = "USB-HOST";
	umdev->cable_set_cb = vuport_cable_set;
	umdev->cable_unset_cb = vuport_cable_unset;

	return intel_usb_mux_register(umdev);
}

static int vuport_remove(struct platform_device *pdev)
{
	return intel_usb_mux_unregister(&pdev->dev);
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
