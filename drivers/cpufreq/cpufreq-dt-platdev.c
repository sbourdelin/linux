/*
 * Copyright (C) 2016 Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/cpufreq-dt.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

struct cpufreq_dt_compat {
	const char *compatible;
	const void *data;
	size_t size;
};

static struct cpufreq_dt_compat compat[] = {
};

static int __init cpufreq_dt_platdev_init(void)
{
	struct platform_device *pdev;
	int i;

	for (i = 0; i < ARRAY_SIZE(compat); i++) {
		if (!of_machine_is_compatible(compat[i].compatible))
			continue;

		pdev = platform_device_register_data(NULL, "cpufreq-dt", -1,
						     compat[i].data,
						     compat[i].size);

		return PTR_ERR_OR_ZERO(pdev);
	}

	return -ENODEV;
}
module_init(cpufreq_dt_platdev_init);

MODULE_ALIAS("cpufreq-dt-platdev");
MODULE_AUTHOR("Viresh Kumar <viresh.kumar@linaro.org>");
MODULE_DESCRIPTION("cpufreq-dt platdev driver");
MODULE_LICENSE("GPL");
