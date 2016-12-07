/*
 * Copyright (c) 2015 ZTE Co., Ltd.
 *           http://www.zte.com.cn
 *
 * Header for ZTE's Power Domain Driver support
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ZTE_PM_DOMAIN_H
#define __ZTE_PM_DOMAIN_H

#include <linux/platform_device.h>
#include <linux/pm_domain.h>

enum {
	REG_CLKEN,
	REG_ISOEN,
	REG_RSTEN,
	REG_PWREN,
	REG_PWRDN,
	REG_ACK_SYNC,

	/* The size of the array - must be last */
	REG_ARRAY_SIZE,
};

enum zx_power_polarity {
	PWREN,
	PWRDN,
};

struct zx_pm_domain {
	struct		generic_pm_domain dm;
	const u16	bit;
	const enum zx_power_polarity	polarity;
	const u16	*reg_offset;
};

extern int zx_normal_power_on(struct generic_pm_domain *domain);
extern int zx_normal_power_off(struct generic_pm_domain *domain);
extern int
zx_pd_probe(struct platform_device *pdev,
	   struct generic_pm_domain **zx_pm_domains,
	   int domain_num);
#endif /* __ZTE_PM_DOMAIN_H */
