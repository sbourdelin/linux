/*
 * Spreadtrum divider clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk-provider.h>

#include "div.h"

DEFINE_SPINLOCK(sprd_div_lock);
EXPORT_SYMBOL_GPL(sprd_div_lock);

long sprd_div_helper_round_rate(struct sprd_clk_common *common,
				const struct sprd_div_internal *div,
				unsigned long rate,
				unsigned long *parent_rate)
{
	return divider_round_rate(&common->hw, rate, parent_rate,
				  NULL, div->width, 0);
}
EXPORT_SYMBOL_GPL(sprd_div_helper_round_rate);

static long sprd_div_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	struct sprd_div *cd = hw_to_sprd_div(hw);

	return sprd_div_helper_round_rate(&cd->common, &cd->div,
					  rate, parent_rate);
}

unsigned long sprd_div_helper_recalc_rate(struct sprd_clk_common *common,
					  const struct sprd_div_internal *div,
					  unsigned long parent_rate)
{
	unsigned long val;
	u32 reg;

	reg = sprd_clk_readl(common);
	val = reg >> div->shift;
	val &= (1 << div->width) - 1;

	return divider_recalc_rate(&common->hw, parent_rate, val, NULL, 0);
}
EXPORT_SYMBOL_GPL(sprd_div_helper_recalc_rate);

static unsigned long sprd_div_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct sprd_div *cd = hw_to_sprd_div(hw);

	return sprd_div_helper_recalc_rate(&cd->common, &cd->div, parent_rate);
}

int sprd_div_helper_set_rate(const struct sprd_clk_common *common,
			     const struct sprd_div_internal *div,
			     unsigned long rate,
			     unsigned long parent_rate)
{
	unsigned long flags;
	unsigned long val;
	u32 reg;

	val = divider_get_val(rate, parent_rate, NULL,
			      div->width, 0);

	spin_lock_irqsave(common->lock, flags);

	reg = sprd_clk_readl(common);
	reg &= ~GENMASK(div->width + div->shift - 1, div->shift);

	sprd_clk_writel(reg | (val << div->shift), common);

	spin_unlock_irqrestore(common->lock, flags);

	return 0;

}
EXPORT_SYMBOL_GPL(sprd_div_helper_set_rate);

static int sprd_div_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct sprd_div *cd = hw_to_sprd_div(hw);

	return sprd_div_helper_set_rate(&cd->common, &cd->div,
					rate, parent_rate);
}

const struct clk_ops sprd_div_ops = {
	.recalc_rate = sprd_div_recalc_rate,
	.round_rate = sprd_div_round_rate,
	.set_rate = sprd_div_set_rate,
};
EXPORT_SYMBOL_GPL(sprd_div_ops);
