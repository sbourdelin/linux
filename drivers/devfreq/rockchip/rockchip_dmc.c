/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 * Author: Lin Huang <hl@rock-chips.com>
 * Base on: https://chromium-review.googlesource.com/#/c/231477/
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

#include <linux/mutex.h>
#include <soc/rockchip/rockchip_dmc.h>

static int num_wait;
static int num_disable;
static BLOCKING_NOTIFIER_HEAD(dmc_notifier_list);
static DEFINE_MUTEX(dmc_en_lock);
static DEFINE_MUTEX(dmc_sync_lock);

void dmc_event(int event)
{
	mutex_lock(&dmc_sync_lock);
	blocking_notifier_call_chain(&dmc_notifier_list, event, NULL);
	mutex_unlock(&dmc_sync_lock);
}
EXPORT_SYMBOL_GPL(dmc_event);

/**
 * rockchip_dmc_enabled - Returns true if dmc freq is enabled, false otherwise.
 */
bool rockchip_dmc_enabled(void)
{
	return num_disable <= 0 && num_wait <= 1;
}
EXPORT_SYMBOL_GPL(rockchip_dmc_enabled);

/**
 * rockchip_dmc_enable - Enable dmc frequency scaling. Will only enable
 * frequency scaling if there are 1 or fewer notifiers. Call to undo
 * rockchip_dmc_disable.
 */
void rockchip_dmc_enable(void)
{
	mutex_lock(&dmc_en_lock);
	num_disable--;
	WARN_ON(num_disable < 0);
	if (rockchip_dmc_enabled())
		dmc_event(DMC_ENABLE);
	mutex_unlock(&dmc_en_lock);
}
EXPORT_SYMBOL_GPL(rockchip_dmc_enable);

/**
 * rockchip_dmc_disable - Disable dmc frequency scaling. Call when something
 * cannot coincide with dmc frequency scaling.
 */
void rockchip_dmc_disable(void)
{
	mutex_lock(&dmc_en_lock);
	if (rockchip_dmc_enabled())
		dmc_event(DMC_DISABLE);
	num_disable++;
	mutex_unlock(&dmc_en_lock);
}
EXPORT_SYMBOL_GPL(rockchip_dmc_disable);

int dmc_register_notifier(struct notifier_block *nb)
{
	int ret;

	if (!nb)
		return -EINVAL;

	ret = blocking_notifier_chain_register(&dmc_notifier_list, nb);

	return ret;
}
EXPORT_SYMBOL_GPL(dmc_register_notifier);

int dmc_unregister_notifier(struct notifier_block *nb)
{
	int ret;

	if (!nb)
		return -EINVAL;

	ret = blocking_notifier_chain_unregister(&dmc_notifier_list, nb);

	return ret;
}
EXPORT_SYMBOL_GPL(dmc_unregister_notifier);

int rockchip_dmc_get(struct notifier_block *nb)
{
	if (!nb)
		return -EINVAL;

	mutex_lock(&dmc_en_lock);

	/* if use two vop, need to disable dmc */
	if (num_wait == 1 && num_disable <= 0)
		dmc_event(DMC_DISABLE);
	num_wait++;
	dmc_register_notifier(nb);
	mutex_unlock(&dmc_en_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(rockchip_dmc_get);

int rockchip_dmc_put(struct notifier_block *nb)
{
	if (!nb)
		return -EINVAL;

	mutex_lock(&dmc_en_lock);
	num_wait--;

	/* from 2 vop back to 1 vop, need enable dmc */
	if (num_wait == 1 && num_disable <= 0)
		dmc_event(DMC_ENABLE);
	dmc_unregister_notifier(nb);
	mutex_unlock(&dmc_en_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(rockchip_dmc_put);
