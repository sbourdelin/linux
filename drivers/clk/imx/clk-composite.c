// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright 2018 NXP
 */

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/clk-provider.h>
#include <linux/clk.h>

#include "clk.h"

#define PCG_PREDIV_SHIFT	16
#define PCG_PREDIV_WIDTH	3

#define PCG_DIV_SHIFT		0
#define PCG_DIV_WIDTH		6

#define PCG_PCS_SHIFT		24
#define PCG_PCS_MASK		0x7

#define PCG_CGC_SHIFT		28

static unsigned long imx_clk_composite_divider_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	unsigned long prediv_rate;
	unsigned int prediv_value;
	unsigned int div_value;

	prediv_value = clk_readl(divider->reg) >> divider->shift;
	prediv_value &= clk_div_mask(divider->width);

	prediv_rate = divider_recalc_rate(hw, parent_rate, prediv_value,
						NULL, divider->flags,
						divider->width);

	div_value = clk_readl(divider->reg) >> PCG_DIV_SHIFT;
	div_value &= clk_div_mask(PCG_DIV_WIDTH);

	return divider_recalc_rate(hw, prediv_rate, div_value, NULL,
				   divider->flags, PCG_DIV_WIDTH);
}

static long imx_clk_composite_divider_round_rate(struct clk_hw *hw,
						unsigned long rate,
						unsigned long *prate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	unsigned long prediv_rate;

	prediv_rate = divider_round_rate(hw, rate, prate, divider->table,
				  divider->width, divider->flags);
	return divider_round_rate(hw, rate, &prediv_rate, divider->table,
				  PCG_DIV_WIDTH, divider->flags);
}

static int imx_clk_composite_divider_set_rate(struct clk_hw *hw,
					unsigned long rate,
					unsigned long parent_rate)
{
	struct clk_divider *divider = to_clk_divider(hw);
	unsigned long prediv_rate;
	unsigned long flags = 0;
	int prediv_value;
	int div_value;
	u32 val;

	prediv_value = divider_get_val(rate, parent_rate, NULL,
				PCG_PREDIV_WIDTH, CLK_DIVIDER_ROUND_CLOSEST);
	if (prediv_value < 0)
		return prediv_value;

	prediv_rate = DIV_ROUND_UP_ULL((u64)parent_rate, prediv_value + 1);

	div_value = divider_get_val(rate, prediv_rate, NULL,
				PCG_DIV_WIDTH, CLK_DIVIDER_ROUND_CLOSEST);
	if (div_value < 0)
		return div_value;

	spin_lock_irqsave(divider->lock, flags);

	val = clk_readl(divider->reg);
	val &= ~((clk_div_mask(divider->width) << divider->shift) |
			(clk_div_mask(PCG_DIV_WIDTH) << PCG_DIV_SHIFT));

	val |= (u32)prediv_value << divider->shift;
	val |= (u32)div_value << PCG_DIV_SHIFT;
	clk_writel(val, divider->reg);

	spin_unlock_irqrestore(divider->lock, flags);

	return 0;
}

static const struct clk_ops imx_clk_composite_divider_ops = {
	.recalc_rate = imx_clk_composite_divider_recalc_rate,
	.round_rate = imx_clk_composite_divider_round_rate,
	.set_rate = imx_clk_composite_divider_set_rate,
};

struct clk *imx_clk_composite_flags(const char *name,
					const char **parent_names,
					int num_parents, void __iomem *reg,
					unsigned long flags)
{
	struct clk_hw *mux_hw = NULL, *div_hw = NULL, *gate_hw = NULL;
	struct clk_divider *div = NULL;
	struct clk_gate *gate = NULL;
	struct clk_mux *mux = NULL;
	struct clk *clk = ERR_PTR(-ENOMEM);

	mux = kzalloc(sizeof(*mux), GFP_KERNEL);
	if (!mux)
		goto fail;

	mux_hw = &mux->hw;
	mux->reg = reg;
	mux->shift = PCG_PCS_SHIFT;
	mux->mask = PCG_PCS_MASK;

	div = kzalloc(sizeof(*div), GFP_KERNEL);
	if (!div)
		goto fail;

	div_hw = &div->hw;
	div->reg = reg;
	div->shift = PCG_PREDIV_SHIFT;
	div->width = PCG_PREDIV_WIDTH;
	div->lock = &imx_ccm_lock;
	div->flags = CLK_DIVIDER_ROUND_CLOSEST;

	gate = kzalloc(sizeof(*gate), GFP_KERNEL);
	if (!gate)
		goto fail;

	gate_hw = &gate->hw;
	gate->reg = reg;
	gate->bit_idx = PCG_CGC_SHIFT;

	clk = clk_register_composite(NULL, name, parent_names, num_parents,
					mux_hw, &clk_mux_ops, div_hw,
					&imx_clk_composite_divider_ops, gate_hw,
					&clk_gate_ops, flags);
	if (IS_ERR(clk))
		goto fail;

	return clk;

fail:
	kfree(gate);
	kfree(div);
	kfree(mux);
	return clk;
}
