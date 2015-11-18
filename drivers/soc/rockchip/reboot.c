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
#include "loader.h"

struct rockchip_reboot {
	struct device *dev;
	struct regmap *map;
	u32 offset;
	struct notifier_block reboot_notifier;
};

static void rockchip_get_reboot_flag(const char *cmd, u32 *flag)
{
	*flag = SYS_LOADER_REBOOT_FLAG + BOOT_NORMAL;

	if (cmd) {
		if (!strcmp(cmd, "loader") || !strcmp(cmd, "bootloader"))
			*flag = SYS_LOADER_REBOOT_FLAG + BOOT_LOADER;
		else if (!strcmp(cmd, "recovery"))
			*flag = SYS_LOADER_REBOOT_FLAG + BOOT_RECOVER;
		else if (!strcmp(cmd, "charge"))
			*flag = SYS_LOADER_REBOOT_FLAG + BOOT_CHARGING;
		else if (!strcmp(cmd, "fastboot"))
			*flag = SYS_LOADER_REBOOT_FLAG + BOOT_FASTBOOT;
	}
}

static int rockchip_reboot_notify(struct notifier_block *this,
				  unsigned long mode, void *cmd)
{
	struct rockchip_reboot *reboot;
	u32 flag;

	reboot = container_of(this, struct rockchip_reboot, reboot_notifier);
	rockchip_get_reboot_flag(cmd, &flag);
	regmap_write(reboot->map, reboot->offset, flag);

	return NOTIFY_DONE;
}

static int __init rockchip_reboot_probe(struct platform_device *pdev)
{
	struct rockchip_reboot *reboot;
	int ret;

	reboot = devm_kzalloc(&pdev->dev, sizeof(*reboot), GFP_KERNEL);
	if (!reboot)
		return -ENOMEM;

	reboot->dev = &pdev->dev;
	reboot->map = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
						      "rockchip,regmap");
	if (IS_ERR(reboot->map))
		return PTR_ERR(reboot->map);

	if (of_property_read_u32(pdev->dev.of_node, "offset", &reboot->offset))
		return -EINVAL;

	reboot->reboot_notifier.notifier_call = rockchip_reboot_notify;
	ret = register_reboot_notifier(&reboot->reboot_notifier);
	if (ret)
		dev_err(reboot->dev, "can't register reboot notifier\n");

	return ret;
}

static const struct of_device_id rockchip_reboot_of_match[] = {
	{ .compatible = "rockchip,reboot" },
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
