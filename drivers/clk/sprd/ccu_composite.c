/*
 * Spreadtrum composite clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk-provider.h>

#include "ccu_composite.h"

DEFINE_SPINLOCK(comp_lock);

static long ccu_comp_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	struct ccu_comp *cc = hw_to_ccu_comp(hw);

	return ccu_div_helper_round_rate(&cc->common, &cc->div,
					 rate, parent_rate);
}

static unsigned long ccu_comp_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct ccu_comp *cc = hw_to_ccu_comp(hw);

	return ccu_div_helper_recalc_rate(&cc->common, &cc->div, parent_rate);
}

static int ccu_comp_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct ccu_comp *cc = hw_to_ccu_comp(hw);

	return ccu_div_helper_set_rate(&cc->common, &cc->div,
				       rate, parent_rate);
}

static u8 ccu_comp_get_parent(struct clk_hw *hw)
{
	struct ccu_comp *cc = hw_to_ccu_comp(hw);

	return ccu_mux_helper_get_parent(&cc->common, &cc->mux);
}

static int ccu_comp_set_parent(struct clk_hw *hw, u8 index)
{
	struct ccu_comp *cc = hw_to_ccu_comp(hw);

	return ccu_mux_helper_set_parent(&cc->common, &cc->mux, index);
}

const struct clk_ops ccu_comp_ops = {
	.get_parent	= ccu_comp_get_parent,
	.set_parent	= ccu_comp_set_parent,

	.round_rate	= ccu_comp_round_rate,
	.recalc_rate	= ccu_comp_recalc_rate,
	.set_rate	= ccu_comp_set_rate,
};
