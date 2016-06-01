/*
 * Copyright (c) 2016 Rockchip Electronics Co. Ltd.
 * Author: Lin Huang <hl@rock-chips.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/of.h>
#include <linux/slab.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include "clk.h"

struct rockchip_ddrclk {
	struct clk_hw	hw;
	void __iomem	*reg_base;
	int		mux_offset;
	int		mux_shift;
	int		mux_width;
	int		mux_flag;
	int		div_shift;
	int		div_width;
	int		div_flag;
	spinlock_t	*lock;
};

#define to_rockchip_ddrclk_hw(hw) container_of(hw, struct rockchip_ddrclk, hw)
#define val_mask(width)	((1 << (width)) - 1)

static int rockchip_ddrclk_set_rate(struct clk_hw *hw, unsigned long drate,
				    unsigned long prate)
{
	struct rockchip_ddrclk *ddrclk = to_rockchip_ddrclk_hw(hw);
	unsigned long flags;

	spin_lock_irqsave(ddrclk->lock, flags);

	/* TODO: set ddr rate in bl31 */

	spin_unlock_irqrestore(ddrclk->lock, flags);

	return 0;
}

static unsigned long
rockchip_ddrclk_recalc_rate(struct clk_hw *hw,
			    unsigned long parent_rate)
{
	struct rockchip_ddrclk *ddrclk = to_rockchip_ddrclk_hw(hw);
	int val;

	val = clk_readl(ddrclk->reg_base +
			ddrclk->mux_offset) >> ddrclk->div_shift;
	val &= val_mask(ddrclk->div_width);

	return DIV_ROUND_UP_ULL((u64)parent_rate, val + 1);
}

static long clk_ddrclk_round_rate(struct clk_hw *hw, unsigned long rate,
				  unsigned long *prate)
{
	return rate;
}

static u8 rockchip_ddrclk_get_parent(struct clk_hw *hw)
{
	struct rockchip_ddrclk *ddrclk = to_rockchip_ddrclk_hw(hw);
	int num_parents = clk_hw_get_num_parents(hw);
	u32 val;

	val = clk_readl(ddrclk->reg_base +
			ddrclk->mux_offset) >> ddrclk->mux_shift;
	val &= val_mask(ddrclk->mux_width);

	if (val >= num_parents)
		return -EINVAL;

	return val;
}

static const struct clk_ops rockchip_ddrclk_ops = {
	.recalc_rate = rockchip_ddrclk_recalc_rate,
	.set_rate = rockchip_ddrclk_set_rate,
	.round_rate = clk_ddrclk_round_rate,
	.get_parent = rockchip_ddrclk_get_parent,
};

struct clk *rockchip_clk_register_ddrclk(const char *name, int flags,
					 const char *const *parent_names,
					 u8 num_parents, int mux_offset,
					 int mux_shift, int mux_width,
					 int mux_flag, int div_shift,
					 int div_width, int div_flag,
					 void __iomem *reg_base,
					 spinlock_t *lock)
{
	struct rockchip_ddrclk *ddrclk;
	struct clk_init_data init;
	struct clk *clk;
	int ret;

	ddrclk = kzalloc(sizeof(*ddrclk), GFP_KERNEL);
	if (!ddrclk)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.parent_names = parent_names;
	init.num_parents = num_parents;
	init.ops = &rockchip_ddrclk_ops;

	init.flags = flags;
	init.flags |= CLK_SET_RATE_NO_REPARENT;
	init.flags |= CLK_GET_RATE_NOCACHE;

	ddrclk->reg_base = reg_base;
	ddrclk->lock = lock;
	ddrclk->hw.init = &init;
	ddrclk->mux_offset = mux_offset;
	ddrclk->mux_shift = mux_shift;
	ddrclk->mux_width = mux_width;
	ddrclk->mux_flag = mux_flag;
	ddrclk->div_shift = div_shift;
	ddrclk->div_width = div_width;
	ddrclk->div_flag = div_flag;

	clk = clk_register(NULL, &ddrclk->hw);
	if (IS_ERR(clk)) {
		pr_err("%s: could not register ddrclk %s\n", __func__,	name);
		ret = PTR_ERR(clk);
		goto free_ddrclk;
	}

	return clk;

free_ddrclk:
	kfree(ddrclk);
	return ERR_PTR(ret);
}
