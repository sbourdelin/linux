/*
 * Spreadtrum divider clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk-provider.h>

#include "ccu_div.h"

DEFINE_SPINLOCK(div_lock);

long ccu_div_helper_round_rate(struct ccu_common *common,
			       struct ccu_div_internal *div,
			       unsigned long rate,
			       unsigned long *parent_rate)
{
	return divider_round_rate(&common->hw, rate, parent_rate,
				  NULL, div->width, 0);
}

static long ccu_div_round_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long *parent_rate)
{
	struct ccu_div *cd = hw_to_ccu_div(hw);

	return ccu_div_helper_round_rate(&cd->common, &cd->div,
					 rate, parent_rate);
}

unsigned long ccu_div_helper_recalc_rate(struct ccu_common *common,
					 struct ccu_div_internal *div,
					 unsigned long parent_rate)
{
	unsigned long val;
	u32 reg;

	reg = ccu_readl(common);
	val = reg >> div->shift;
	val &= (1 << div->width) - 1;

	return divider_recalc_rate(&common->hw, parent_rate, val, NULL, 0);
}

static unsigned long ccu_div_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	struct ccu_div *cd = hw_to_ccu_div(hw);

	return ccu_div_helper_recalc_rate(&cd->common, &cd->div, parent_rate);
}

int ccu_div_helper_set_rate(struct ccu_common *common,
			    struct ccu_div_internal *div,
			    unsigned long rate,
			    unsigned long parent_rate)
{
	unsigned long flags;
	unsigned long val;
	u32 reg;

	val = divider_get_val(rate, parent_rate, NULL,
				div->width, 0);

	spin_lock_irqsave(common->lock, flags);

	reg = ccu_readl(common);
	reg &= ~GENMASK(div->width + div->shift - 1, div->shift);

	ccu_writel(reg | (val << div->shift), common);

	spin_unlock_irqrestore(common->lock, flags);

	return 0;

}

static int ccu_div_set_rate(struct clk_hw *hw, unsigned long rate,
			    unsigned long parent_rate)
{
	struct ccu_div *cd = hw_to_ccu_div(hw);

	return ccu_div_helper_set_rate(&cd->common, &cd->div,
				       rate, parent_rate);
}

const struct clk_ops ccu_div_ops = {
	.recalc_rate = ccu_div_recalc_rate,
	.round_rate = ccu_div_round_rate,
	.set_rate = ccu_div_set_rate,
};
