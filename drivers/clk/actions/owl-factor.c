/*
 * Copyright (c) 2014 Actions Semi Inc.
 * Author: David Liu <liuwei@actions-semi.com>
 *
 * Copyright (c) 2017 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/clk-provider.h>
#include <linux/slab.h>
#include "owl-clk.h"

#define to_owl_factor(_hw)	container_of(_hw, struct owl_factor, hw)
#define div_mask(d)		((1 << ((d)->width)) - 1)

static unsigned int _get_table_maxval(const struct clk_factor_table *table)
{
	unsigned int maxval = 0;
	const struct clk_factor_table *clkt;

	for (clkt = table; clkt->div; clkt++)
		if (clkt->val > maxval)
			maxval = clkt->val;
	return maxval;
}

static int _get_table_div_mul(const struct clk_factor_table *table,
			unsigned int val, unsigned int *mul, unsigned int *div)
{
	const struct clk_factor_table *clkt;

	for (clkt = table; clkt->div; clkt++) {
		if (clkt->val == val) {
			*mul = clkt->mul;
			*div = clkt->div;
			return 1;
		}
	}
	return 0;
}

static unsigned int _get_table_val(const struct clk_factor_table *table,
			unsigned long rate, unsigned long parent_rate)
{
	const struct clk_factor_table *clkt;
	int val = -1;
	u64 calc_rate;

	for (clkt = table; clkt->div; clkt++) {
		calc_rate = parent_rate * clkt->mul;
		do_div(calc_rate, clkt->div);

		if ((unsigned long)calc_rate <= rate) {
			val = clkt->val;
			break;
		}
	}

	if (val == -1)
		val = _get_table_maxval(table);

	return val;
}

static int clk_val_best(struct clk_hw *hw, unsigned long rate,
			unsigned long *best_parent_rate)
{
	struct owl_factor *factor = to_owl_factor(hw);
	const struct clk_factor_table *clkt = factor->table;
	unsigned long parent_rate, try_parent_rate, best = 0, cur_rate;
	unsigned long parent_rate_saved = *best_parent_rate;
	int bestval = 0;

	if (!rate)
		rate = 1;

	if (!(clk_hw_get_flags(hw) & CLK_SET_RATE_PARENT)) {
		parent_rate = *best_parent_rate;
		bestval = _get_table_val(clkt, rate, parent_rate);
		return bestval;
	}

	for (clkt = factor->table; clkt->div; clkt++) {
		try_parent_rate = rate * clkt->div / clkt->mul;

		if (try_parent_rate == parent_rate_saved) {
			pr_debug("%s: [%d %d %d] found try_parent_rate %ld\n",
				__func__, clkt->val, clkt->mul, clkt->div,
				try_parent_rate);
			/*
			 * It's the most ideal case if the requested rate can be
			 * divided from parent clock without any need to change
			 * parent rate, so return the divider immediately.
			 */
			*best_parent_rate = parent_rate_saved;
			return clkt->val;
		}

		parent_rate = clk_hw_round_rate(clk_hw_get_parent(hw),
				try_parent_rate);
		cur_rate = DIV_ROUND_UP(parent_rate, clkt->div) * clkt->mul;
		if (cur_rate <= rate && cur_rate > best) {
			bestval = clkt->val;
			best = cur_rate;
			*best_parent_rate = parent_rate;
		}
	}

	if (!bestval) {
		bestval = _get_table_maxval(clkt);
		*best_parent_rate = clk_hw_round_rate(
				clk_hw_get_parent(hw), 1);
	}

	pr_debug("%s: return bestval %d\n", __func__, bestval);

	return bestval;
}

/**
 * owl_factor_round_rate() - round a clock frequency
 * @hw:	handle between common and hardware-specific interfaces
 * @rate: desired clock frequency
 * @prate: clock frequency of parent clock
 */
static long owl_factor_round_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long *parent_rate)
{
	struct owl_factor *factor = to_owl_factor(hw);
	const struct clk_factor_table *clkt = factor->table;
	unsigned int val, mul = 0, div = 1;

	val = clk_val_best(hw, rate, parent_rate);
	_get_table_div_mul(clkt, val, &mul, &div);

	return *parent_rate * mul / div;
}

/**
 * owl_factor_recalc_rate() - recalculate clock frequency
 * @hw:	handle between common and hardware-specific interfaces
 * @parent_rate: clock frequency of parent clock
 */
static unsigned long owl_factor_recalc_rate(struct clk_hw *hw,
			unsigned long parent_rate)
{
	struct owl_factor *factor = to_owl_factor(hw);
	const struct clk_factor_table *clkt = factor->table;
	u64 rate;
	u32 val, mul, div;

	div = 0;
	mul = 0;

	val = readl(factor->reg) >> factor->shift;
	val &= div_mask(factor);

	_get_table_div_mul(clkt, val, &mul, &div);
	if (!div) {
		WARN(!(factor->flags & CLK_DIVIDER_ALLOW_ZERO),
			"%s: Zero divisor and CLK_DIVIDER_ALLOW_ZERO not set\n",
			__clk_get_name(hw->clk));
		return parent_rate;
	}

	rate = (u64)parent_rate * mul;
	do_div(rate, div);

	return rate;
}

/**
 * owl_factor_set_rate() - set clock frequency
 * @hw: handle between common and hardware-specific interfaces
 * @parent_rate: clock frequency of parent clock
 */
static int owl_factor_set_rate(struct clk_hw *hw, unsigned long rate,
			unsigned long parent_rate)
{
	struct owl_factor *factor = to_owl_factor(hw);
	unsigned long flags = 0;
	u32 val, v;

	val = _get_table_val(factor->table, rate, parent_rate);

	pr_debug("%s: get_table_val %d\n", __func__, val);

	if (val > div_mask(factor))
		val = div_mask(factor);

	if (factor->lock)
		spin_lock_irqsave(factor->lock, flags);

	v = readl(factor->reg);
	v &= ~(div_mask(factor) << factor->shift);
	v |= val << factor->shift;
	writel(v, factor->reg);

	if (factor->lock)
		spin_unlock_irqrestore(factor->lock, flags);

	return 0;
}

const struct clk_ops owl_factor_ops = {
	.round_rate	= owl_factor_round_rate,
	.recalc_rate	= owl_factor_recalc_rate,
	.set_rate	= owl_factor_set_rate,
};

/**
 * owl_factor_clk_register() - register pll with the clock framework
 * @name: pll name
 * @parent: parent clock name
 * @reg: pointer to pll control register
 * @pll_status: pointer to pll status register
 * @lock_index: bit index to this pll's lock status bit in pll_status
 * @lock: register lock
 */
struct clk_hw *owl_factor_clk_register(struct device *dev, const char *name,
		const char *parent_name, unsigned long flags,
		void __iomem *reg, u8 shift, u8 width,
		u8 clk_factor_flags, const struct clk_factor_table *table,
		spinlock_t *lock)

{
	struct owl_factor *factor;
	struct clk_init_data initd;
	struct clk_hw *clk_hw;
	int ret;

	factor = kzalloc(sizeof(*factor), GFP_KERNEL);
	if (!factor)
		return ERR_PTR(-ENOMEM);

	initd.name = name;
	initd.ops = &owl_factor_ops;
	initd.flags = flags;
	initd.parent_names = (parent_name ? &parent_name : NULL);
	initd.num_parents = (parent_name ? 1 : 0);

	factor->reg = reg;
	factor->shift = shift;
	factor->width = width;
	factor->flags = clk_factor_flags;
	factor->lock = lock;
	factor->hw.init = &initd;
	factor->table = table;

	clk_hw = &factor->hw;
	ret = clk_hw_register(dev, clk_hw);
	if (ret) {
		kfree(factor);
		clk_hw = ERR_PTR(ret);
	}

	return clk_hw;
}
