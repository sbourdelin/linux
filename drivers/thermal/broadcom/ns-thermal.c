/*
 * Copyright (C) 2017 Rafał Miłecki <rafal@milecki.pl>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/thermal.h>

#define PVTMON_CONTROL0					0x00
#define PVTMON_CONTROL0_SEL_MASK			0x0000000e
#define PVTMON_CONTROL0_SEL_TEMP_MONITOR		0x00000000
#define PVTMON_CONTROL0_SEL_TEST_MODE			0x0000000e
#define PVTMON_STATUS					0x08

struct ns_thermal {
	void __iomem *pvtmon;
};

static int ns_thermal_get_temp(void *data, int *temp)
{
	struct ns_thermal *ns_thermal = data;
	u32 val;

	val = readl(ns_thermal->pvtmon + PVTMON_CONTROL0);
	if ((val & PVTMON_CONTROL0_SEL_MASK) != PVTMON_CONTROL0_SEL_TEMP_MONITOR) {
		val &= ~PVTMON_CONTROL0_SEL_MASK;
		val |= PVTMON_CONTROL0_SEL_TEMP_MONITOR;
		writel(val, ns_thermal->pvtmon + PVTMON_CONTROL0);
	}

	val = readl(ns_thermal->pvtmon + PVTMON_STATUS);
	*temp = (418 - (val * 5556 / 10000)) * 1000;

	return 0;
}

const struct thermal_zone_of_device_ops ns_thermal_ops = {
	.get_temp = ns_thermal_get_temp,
};

static int ns_thermal_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ns_thermal *ns_thermal;
	struct thermal_zone_device *tzd;

	ns_thermal = devm_kzalloc(dev, sizeof(*ns_thermal), GFP_KERNEL);
	if (!ns_thermal)
		return -ENOMEM;

	ns_thermal->pvtmon = of_iomap(dev_of_node(dev), 0);
	if (WARN_ON(!ns_thermal->pvtmon))
		return -ENOENT;

	tzd = devm_thermal_zone_of_sensor_register(dev, 0, ns_thermal,
						   &ns_thermal_ops);
	if (IS_ERR(tzd)) {
		iounmap(ns_thermal->pvtmon);
		return PTR_ERR(tzd);
	}

	platform_set_drvdata(pdev, ns_thermal);

	return 0;
}

static int ns_thermal_remove(struct platform_device *pdev)
{
	struct ns_thermal *ns_thermal = platform_get_drvdata(pdev);

	iounmap(ns_thermal->pvtmon);

	return 0;
}

static const struct of_device_id ns_thermal_of_match[] = {
	{ .compatible = "brcm,ns-thermal", },
	{},
};
MODULE_DEVICE_TABLE(of, ns_thermal_of_match);

static struct platform_driver ns_thermal_driver = {
	.probe		= ns_thermal_probe,
	.remove		= ns_thermal_remove,
	.driver = {
		.name = "ns-thermal",
		.of_match_table = ns_thermal_of_match,
	},
};
module_platform_driver(ns_thermal_driver);

MODULE_DESCRIPTION("Northstar thermal driver");
MODULE_LICENSE("GPL v2");
