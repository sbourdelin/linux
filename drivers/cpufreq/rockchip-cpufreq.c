/*
 * Rockchip Platforms CPUFreq Support
 *
 * Copyright (C) 2016 Fuzhou Rockchip Electronics Co., Ltd
 *
 * Feng Xiao <xf@rock-chips.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

static const char * const rockchip_compat[] = {
	"rockchip,rk2928",
	"rockchip,rk3066a",
	"rockchip,rk3066b",
	"rockchip,rk3188",
	"rockchip,rk3288",
	"rockchip,rk3366",
	"rockchip,rk3368",
	"rockchip,rk3399",
};

static int __init rockchip_cpufreq_driver_init(void)
{
	struct platform_device *pdev;
	int i;

	for (i = 0; i < ARRAY_SIZE(rockchip_compat); i++) {
		if (of_machine_is_compatible(rockchip_compat[i])) {
			pdev = platform_device_register_simple("cpufreq-dt",
							       -1, NULL, 0);
			return PTR_ERR_OR_ZERO(pdev);
		}
	}

	return -ENODEV;
}
module_init(rockchip_cpufreq_driver_init);

MODULE_AUTHOR("Feng Xiao <xf@rock-chips.com>");
MODULE_DESCRIPTION("Rockchip cpufreq driver");
MODULE_LICENSE("GPL v2");
