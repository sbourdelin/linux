/*
 * include/linux/cpu-pd.h
 *
 * Copyright (C) 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __CPU_PD_H__
#define __CPU_PD_H__

#include <linux/cpumask.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/pm_domain.h>

struct cpu_pd_ops {
	int (*power_off)(struct generic_pm_domain *genpd);
	int (*power_on)(struct generic_pm_domain *genpd);
};

struct cpu_pm_domain {
	struct list_head link;
	struct generic_pm_domain *genpd;
	struct device_node *dn;
	struct cpu_pd_ops plat_ops;
	cpumask_var_t cpus;
};

struct generic_pm_domain *of_init_cpu_pm_domain(struct device_node *dn,
		const struct cpu_pd_ops *ops);
int of_attach_cpu_pm_domain(struct device_node *dn);
#endif /* __CPU_PD_H__ */
