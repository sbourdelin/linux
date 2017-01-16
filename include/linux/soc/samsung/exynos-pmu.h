/*
 * Copyright (c) 2014 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Header for EXYNOS PMU Driver support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __LINUX_SOC_EXYNOS_PMU_H
#define __LINUX_SOC_EXYNOS_PMU_H

#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

enum sys_powerdown {
	SYS_AFTR,
	SYS_LPA,
	SYS_SLEEP,
	NUM_SYS_POWERDOWN,
};

extern void exynos_sys_powerdown_conf(enum sys_powerdown mode);

#define EXYNOS_PMU_DEV_NAME "exynos-pmu"

static inline struct regmap *exynos_get_pmu_regs(void)
{
	struct device *dev = bus_find_device_by_name(&platform_bus_type, NULL,
						     EXYNOS_PMU_DEV_NAME);
	if (dev) {
		struct regmap *regs = syscon_node_to_regmap(dev->of_node);
		put_device(dev);
		if (!IS_ERR(regs))
			return regs;
	}
	return NULL;
}

#endif /* __LINUX_SOC_EXYNOS_PMU_H */
