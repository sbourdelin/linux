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
#include <linux/io.h>
#include <linux/delay.h>
#include "owl-clk.h"

/**
 * struct owl_pll
 * @hw: handle between common and hardware-specific interfaces
 * @reg: pll control register
 * @lock: register lock
 * @bfreq: base frequency of the pll. pll frequency = bfreq * mul
 * @enable_bit: enable bit for pll
 * @shift: shift to the multiplier bit field
 * @width: width of the multiplier bit field
 * @min_mul: minimum multiple for the pll
 * @max_mul: maximum multiple for the pll
 * @pll_flags: flags for the pll
 * @table: pll table
 */
struct owl_pll {
	struct clk_hw		hw;
	void __iomem		*reg;
	spinlock_t		*lock;
	unsigned long		bfreq;
	u8			enable_bit;
	u8			shift;
	u8			width;
	u8			min_mul;
	u8			max_mul;
	u8			pll_flags;
	const struct clk_pll_table *table;
};

#define to_owl_pll(_hw)		container_of(_hw, struct owl_pll, hw)
#define mul_mask(m)		((1 << ((m)->width)) - 1)
#define PLL_STABILITY_WAIT_US	(50)

/**
 * owl_pll_calculate_mul() - calculate multiple for specific rate
 * @pll: owl pll
 * @rate: desired clock frequency
 */
static u32 owl_pll_calculate_mul(struct owl_pll *pll, unsigned long rate)
{
	u32 mul;

	mul = DIV_ROUND_CLOSEST(rate, pll->bfreq);
	if (mul < pll->min_mul)
		mul = pll->min_mul;
	else if (mul > pll->max_mul)
		mul = pll->max_mul;

	return mul &= mul_mask(pll);
}

static unsigned int _get_table_rate(const struct clk_pll_table *table,
		unsigned int val)
{
	const struct clk_pll_table *clkt;

	for (clkt = table; clkt->rate; clkt++)
		if (clkt->val == val)
			return clkt->rate;

	return 0;
}

static const struct clk_pll_table *_get_pll_table(
		const struct clk_pll_table *table, unsigned long rate)
{
	const struct clk_pll_table *clkt;

	for (clkt = table; clkt->rate; clkt++) {
		if (clkt->rate == rate) {
			table = clkt;
			break;
		} else if (clkt->rate < rate)
			table = clkt;
	}

	return table;
}

/**
 * owl_pll_round_rate() - round a clock frequency
 * @hw: handle between common and hardware-specific interfaces
 * @rate: desired clock frequency
 * @parent_rate: clock frequency of parent clock
 */
static long owl_pll_round_rate(struct clk_hw *hw, unsigned long rate,
		unsigned long *parent_rate)
{
	struct owl_pll *pll = to_owl_pll(hw);
	const struct clk_pll_table *clkt;
	u32 mul;

	if (pll->table) {
		clkt = _get_pll_table(pll->table, rate);
		return clkt->rate;
	}

	/* fixed frequency */
	if (pll->width == 0)
		return pll->bfreq;

	mul = owl_pll_calculate_mul(pll, rate);

	return pll->bfreq * mul;
}

/**
 * owl_pll_recalc_rate() - recalculate pll clock frequency
 * @hw:	handle between common and hardware-specific interfaces
 * @parent_rate: clock frequency of parent clock
 */
static unsigned long owl_pll_recalc_rate(struct clk_hw *hw,
		unsigned long parent_rate)
{
	struct owl_pll *pll = to_owl_pll(hw);
	unsigned long rate;
	u32 val, mul;

	if (pll->table) {
		val = readl(pll->reg) >> pll->shift;
		val &= mul_mask(pll);

		rate = _get_table_rate(pll->table, val);

		return rate;
	}

	/* fixed frequency */
	if (pll->width == 0)
		return pll->bfreq;

	mul = (readl(pll->reg) >> pll->shift) & mul_mask(pll);

	return pll->bfreq * mul;
}

/**
 * owl_pll_is_enabled - check if pll is enabled
 * @hw: handle between common and hardware-specific interfaces
 *
 * Not sure this is a good idea, but since disabled means bypassed for
 * this clock implementation we say we are always enabled.
 */
static int owl_pll_is_enabled(struct clk_hw *hw)
{
	struct owl_pll *pll = to_owl_pll(hw);
	unsigned long flags = 0;
	u32 v;

	if (pll->lock)
		spin_lock_irqsave(pll->lock, flags);

	v = readl(pll->reg);

	if (pll->lock)
		spin_unlock_irqrestore(pll->lock, flags);

	return !!(v & BIT(pll->enable_bit));
}

/**
 * owl_pll_enable - enable pll clock
 * @hw:	handle between common and hardware-specific interfaces
 */
static int owl_pll_enable(struct clk_hw *hw)
{
	struct owl_pll *pll = to_owl_pll(hw);
	unsigned long flags = 0;
	u32 v;

	/* exit if pll is enabled */
	if (owl_pll_is_enabled(hw))
		return 0;

	if (pll->lock)
		spin_lock_irqsave(pll->lock, flags);

	v = readl(pll->reg);
	v |= BIT(pll->enable_bit);
	writel(v, pll->reg);

	if (pll->lock)
		spin_unlock_irqrestore(pll->lock, flags);

	udelay(PLL_STABILITY_WAIT_US);

	return 0;
}

/**
 * owl_pll_disable - disable pll clock
 * @hw:	handle between common and hardware-specific interfaces
 */
static void owl_pll_disable(struct clk_hw *hw)
{
	struct owl_pll *pll = to_owl_pll(hw);
	unsigned long flags = 0;
	u32 v;

	/* exit if pll is disabled */
	if (!owl_pll_is_enabled(hw))
		return;

	if (pll->lock)
		spin_lock_irqsave(pll->lock, flags);

	v = readl(pll->reg);
	v &= ~BIT(pll->enable_bit);
	writel(v, pll->reg);

	if (pll->lock)
		spin_unlock_irqrestore(pll->lock, flags);
}

/**
 * owl_pll_set_rate - set pll rate
 * @hw: handle between common and hardware-specific interfaces
 * @rate: desired clock frequency
 * @parent_rate: clock frequency of parent
 */
static int owl_pll_set_rate(struct clk_hw *hw, unsigned long rate,
		unsigned long parent_rate)
{
	struct owl_pll *pll = to_owl_pll(hw);
	const struct clk_pll_table *clkt;
	unsigned long flags = 0;
	u32 val, v;

	pr_debug("%s: rate %ld, parent_rate %ld, before set rate: reg 0x%x\n",
		__func__, rate, parent_rate, readl(pll->reg));

	/* fixed frequency */
	if (pll->width == 0)
		return 0;

	if (pll->table) {
		clkt = _get_pll_table(pll->table, rate);
		val = clkt->val;
	} else {
		val = owl_pll_calculate_mul(pll, rate);
	}

	if (pll->lock)
		spin_lock_irqsave(pll->lock, flags);

	v = readl(pll->reg);
	v &= ~mul_mask(pll);
	v |= val << pll->shift;
	writel(v, pll->reg);

	udelay(PLL_STABILITY_WAIT_US);

	if (pll->lock)
		spin_unlock_irqrestore(pll->lock, flags);

	pr_debug("%s: after set rate: reg 0x%x\n", __func__,
		readl(pll->reg));

	return 0;
}

static const struct clk_ops owl_pll_ops = {
	.enable = owl_pll_enable,
	.disable = owl_pll_disable,
	.is_enabled = owl_pll_is_enabled,
	.round_rate = owl_pll_round_rate,
	.recalc_rate = owl_pll_recalc_rate,
	.set_rate = owl_pll_set_rate,
};

/**
 * owl_pll_clk_register() - register pll with the clock framework
 * @name: pll name
 * @parent: parent clock name
 * @reg: pointer to pll control register
 * @pll_status: pointer to pll status register
 * @lock_index: bit index to this pll's lock status bit in @pll_status
 * @lock: register lock
 */
struct clk_hw *owl_pll_clk_register(const char *name, const char *parent_name,
		unsigned long flags, void __iomem *reg, unsigned long bfreq,
		u8 enable_bit, u8 shift, u8 width, u8 min_mul, u8 max_mul,
		u8 pll_flags, const struct clk_pll_table *table,
		spinlock_t *lock)
{
	struct owl_pll *pll;
	struct clk_hw *clk_hw;
	struct clk_init_data initd;
	int ret;

	pll = kmalloc(sizeof(*pll), GFP_KERNEL);
	if (!pll)
		return ERR_PTR(-ENOMEM);

	initd.name = name;
	initd.parent_names = (parent_name ? &parent_name : NULL);
	initd.num_parents = (parent_name ? 1 : 0);
	initd.ops = &owl_pll_ops;
	initd.flags = flags;

	pll->hw.init = &initd;
	pll->bfreq = bfreq;
	pll->enable_bit = enable_bit;
	pll->shift = shift;
	pll->width = width;
	pll->min_mul = min_mul;
	pll->max_mul = max_mul;
	pll->pll_flags = pll_flags;
	pll->table = table;
	pll->reg = reg;
	pll->lock = lock;

	clk_hw = &pll->hw;
	ret = clk_hw_register(NULL, clk_hw);
	if (ret) {
		kfree(pll);
		clk_hw = ERR_PTR(ret);
	}

	return clk_hw;
}
