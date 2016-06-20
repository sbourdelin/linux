/*
 * Copyright (C) 2016 Applied Micro Circuits Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Fractional scale clock is implemented for a single register field.
 *
 * Output rate = parent_rate * scale / denominator
 *
 * For example, for 1/8 fractional scale, denominator will be 8 and scale
 * will be computed and programmed accordingly.
 */

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#define to_clk_sf(_hw) container_of(_hw, struct clk_fractional_scale, hw)

static unsigned long clk_fs_recalc_rate(struct clk_hw *hw,
					unsigned long parent_rate)
{
	struct clk_fractional_scale *fd = to_clk_sf(hw);
	unsigned long flags = 0;
	u64 ret, scale;
	u32 val;

	if (fd->lock)
		spin_lock_irqsave(fd->lock, flags);
	else
		__acquire(fd->lock);

	val = clk_readl(fd->reg);

	if (fd->lock)
		spin_unlock_irqrestore(fd->lock, flags);
	else
		__release(fd->lock);

	ret = (u64) parent_rate;

	scale = (val & fd->mask) >> fd->shift;
	if (fd->flags & CLK_FRACTIONAL_SCALE_INVERTED)
		scale = fd->denom - scale;
	else
		scale++;

	/* freq = parent_rate * scaler / denom */
	do_div(ret, fd->denom);
	ret *= scale;
	if (ret == 0)
		ret = (u64) parent_rate;

	return ret;
}

static long clk_fs_round_rate(struct clk_hw *hw, unsigned long rate,
			      unsigned long *parent_rate)
{
	struct clk_fractional_scale *fd = to_clk_sf(hw);
	u64 ret, scale;

	if (!rate || rate >= *parent_rate)
		return *parent_rate;

	/* freq = parent_rate * scaler / denom */
	ret = rate * fd->denom;
	scale = DIV_ROUND_UP_ULL(ret, *parent_rate);

	ret = (u64) *parent_rate * scale;
	do_div(ret, fd->denom);

	return ret;
}

static int clk_fs_set_rate(struct clk_hw *hw, unsigned long rate,
			   unsigned long parent_rate)
{
	struct clk_fractional_scale *fd = to_clk_sf(hw);
	unsigned long flags = 0;
	u64 scale, ret;
	u32 val;

	/*
	 * Compute the scaler:
	 *
	 * freq = parent_rate * scaler / denom, or
	 * scaler = freq * denom / parent_rate
	 */
	ret = rate * fd->denom;
	scale = DIV_ROUND_UP_ULL(ret, (u64)parent_rate);

	/* Check if inverted */
	if (fd->flags & CLK_FRACTIONAL_SCALE_INVERTED)
		scale = fd->denom - scale;
	else
		scale--;

	if (fd->lock)
		spin_lock_irqsave(fd->lock, flags);
	else
		__acquire(fd->lock);

	val = clk_readl(fd->reg);
	val &= ~fd->mask;
	val |= (scale << fd->shift);
	clk_writel(val, fd->reg);

	if (fd->lock)
		spin_unlock_irqrestore(fd->lock, flags);
	else
		__release(fd->lock);

	return 0;
}

const struct clk_ops clk_fractional_scale_ops = {
	.recalc_rate = clk_fs_recalc_rate,
	.round_rate = clk_fs_round_rate,
	.set_rate = clk_fs_set_rate,
};
EXPORT_SYMBOL_GPL(clk_fractional_scale_ops);

struct clk_hw *clk_hw_register_fractional_scale(struct device *dev,
		const char *name, const char *parent_name, unsigned long flags,
		void __iomem *reg, u8 shift, u8 width, u64 denom,
		u32 clk_flags, spinlock_t *lock)
{
	struct clk_fractional_scale *fd;
	struct clk_init_data init;
	struct clk_hw *hw;
	int ret;

	fd = kzalloc(sizeof(*fd), GFP_KERNEL);
	if (!fd)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = &clk_fractional_scale_ops;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;

	fd->reg = reg;
	fd->shift = shift;
	fd->mask = (BIT(width) - 1) << shift;
	fd->denom = denom;
	fd->flags = clk_flags;
	fd->lock = lock;
	fd->hw.init = &init;

	hw = &fd->hw;
	ret = clk_hw_register(dev, hw);
	if (ret) {
		kfree(fd);
		hw = ERR_PTR(ret);
	}

	return hw;
}
EXPORT_SYMBOL_GPL(clk_hw_register_fractional_scale);

struct clk *clk_register_fractional_scale(struct device *dev,
		const char *name, const char *parent_name, unsigned long flags,
		void __iomem *reg, u8 shift, u8 width, u64 denom,
		u32 clk_flags, spinlock_t *lock)
{
	struct clk_hw *hw;

	hw = clk_hw_register_fractional_scale(dev, name, parent_name, flags,
					      reg, shift, width, denom,
					      clk_flags, lock);
	if (IS_ERR(hw))
		return ERR_CAST(hw);

	return hw->clk;
}
EXPORT_SYMBOL_GPL(clk_register_fractional_scale);

void clk_hw_unregister_fractional_scale(struct clk_hw *hw)
{
	struct clk_fractional_scale *fd;

	fd = to_clk_sf(hw);

	clk_hw_unregister(hw);
	kfree(fd);
}
EXPORT_SYMBOL_GPL(clk_hw_unregister_fractional_scale);
