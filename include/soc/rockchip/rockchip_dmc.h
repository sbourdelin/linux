/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef __SOC_ROCKCHIP_DMC_H
#define __SOC_ROCKCHIP_DMC_H

#include <linux/notifier.h>

#define DMC_ENABLE	0
#define DMC_DISABLE	1
#define DMCFREQ_ADJUST	2
#define DMCFREQ_FINISH	3

#if IS_ENABLED(CONFIG_ARM_ROCKCHIP_DMC_DEVFREQ)
int rockchip_dmc_get(struct notifier_block *nb);
int rockchip_dmc_put(struct notifier_block *nb);
#else
static inline int rockchip_dmc_get(struct notifier_block *nb)
{
	return 0;
}

static inline int rockchip_dmc_put(struct notifier_block *nb)
{
	return 0;
}
#endif

void dmc_event(int event);
int dmc_register_notifier(struct notifier_block *nb);
int dmc_unregister_notifier(struct notifier_block *nb);
void rockchip_dmc_enable(void);
void rockchip_dmc_disable(void);
bool rockchip_dmc_enabled(void);
#endif
