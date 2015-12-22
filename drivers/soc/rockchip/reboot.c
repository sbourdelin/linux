/*
 * Copyright (c) 2014, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon.h>
#include <dt-bindings/soc/rockchip_boot-mode.h>

static struct regmap *map;
static u32 offset;

static int rockchip_reboot_mode_write(int magic)
{
	if (!magic)
		magic = BOOT_NORMAL;

	regmap_write(map, offset, magic);

	return 0;
}

static int rockchip_reboot_probe(struct platform_device *pdev)
{
	int ret;

	map = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
					      "rockchip,regmap");
	if (IS_ERR(map))
		return PTR_ERR(map);

	if (of_property_read_u32(pdev->dev.of_node, "offset", &offset))
		return -EINVAL;

	ret = reboot_mode_register(&pdev->dev, rockchip_reboot_mode_write);
	if (ret)
		dev_err(&pdev->dev, "can't register reboot mode\n");

	return ret;
}

static const struct of_device_id rockchip_reboot_of_match[] = {
	{ .compatible = "rockchip,reboot-mode" },
	{}
};

static struct platform_driver rockchip_reboot_driver = {
	.probe = rockchip_reboot_probe,
	.driver = {
		.name = "rockchip-reboot",
		.of_match_table = rockchip_reboot_of_match,
	},
};
module_platform_driver(rockchip_reboot_driver);

MODULE_AUTHOR("Andy Yan <andy.yan@rock-chips.com");
MODULE_DESCRIPTION("Rockchip platform reboot notifier driver");
MODULE_LICENSE("GPL");
