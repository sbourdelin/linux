/*
 * linux/drivers/media/platform/s5p-mfc/s5p_mfc_pm.c
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include "s5p_mfc_common.h"
#include "s5p_mfc_debug.h"
#include "s5p_mfc_pm.h"

static struct s5p_mfc_pm *pm;
static struct s5p_mfc_dev *p_dev;
static atomic_t clk_ref;

int s5p_mfc_init_pm(struct s5p_mfc_dev *dev)
{
	int ret;

	pm = &dev->pm;
	p_dev = dev;

	pm->num_clocks = dev->variant->num_clocks;
	pm->clk_names = dev->variant->clk_names;
	pm->device = &dev->plat_dev->dev;
	pm->clock_gate = NULL;

	/* clock control */
	pm->clocks = devm_clk_bulk_alloc(pm->device, pm->num_clocks,
					 pm->clk_names);
	if (IS_ERR(pm->clocks))
		return PTR_ERR(pm->clocks);

	ret = devm_clk_bulk_get(pm->device, pm->num_clocks, pm->clocks);
	if (ret < 0)
		return ret;

	if (dev->variant->use_clock_gating)
		pm->clock_gate = pm->clocks[0].clk;

	pm_runtime_enable(pm->device);
	atomic_set(&clk_ref, 0);
	return 0;
}

void s5p_mfc_final_pm(struct s5p_mfc_dev *dev)
{
	pm_runtime_disable(pm->device);
}

int s5p_mfc_clock_on(void)
{
	atomic_inc(&clk_ref);
	mfc_debug(3, "+ %d\n", atomic_read(&clk_ref));

	return clk_enable(pm->clock_gate);
}

void s5p_mfc_clock_off(void)
{
	atomic_dec(&clk_ref);
	mfc_debug(3, "- %d\n", atomic_read(&clk_ref));

	clk_disable(pm->clock_gate);
}

int s5p_mfc_power_on(void)
{
	int ret = 0;

	ret = pm_runtime_get_sync(pm->device);
	if (ret < 0)
		return ret;

	/* clock control */
	ret = clk_bulk_prepare_enable(pm->num_clocks, pm->clocks);
	if (ret < 0)
		goto err;

	/* prepare for software clock gating */
	clk_disable(pm->clock_gate);

	return 0;
err:
	pm_runtime_put(pm->device);
	return ret;
}

int s5p_mfc_power_off(void)
{
	/* finish software clock gating */
	clk_enable(pm->clock_gate);

	clk_bulk_disable_unprepare(pm->num_clocks, pm->clocks);

	return pm_runtime_put_sync(pm->device);
}

