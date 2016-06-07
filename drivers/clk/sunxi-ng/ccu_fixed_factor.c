/*
 * Copyright (C) 2016 Maxime Ripard
 * Maxime Ripard <maxime.ripard@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/clk-provider.h>

#include "ccu_fixed_factor.h"

static unsigned long ccu_fixed_factor_recalc_rate(struct clk_hw *hw,
						  unsigned long parent_rate)
{
	struct ccu_fixed_factor *fix = hw_to_ccu_fixed_factor(hw);

	return parent_rate / fix->div * fix->mult;
}

static long ccu_fixed_factor_round_rate(struct clk_hw *hw,
					unsigned long rate,
					unsigned long *parent_rate)
{
	struct ccu_fixed_factor *fix = hw_to_ccu_fixed_factor(hw);

	if (clk_hw_get_flags(hw) & CLK_SET_RATE_PARENT) {
		unsigned long best_parent;

		best_parent = (rate / fix->mult) * fix->div;
		*parent_rate = clk_hw_round_rate(clk_hw_get_parent(hw),
						 best_parent);
	}

	return *parent_rate / fix->div * fix->mult;
}

static int ccu_fixed_factor_set_rate(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	return 0;
}

const struct clk_ops ccu_fixed_factor_ops = {
	.recalc_rate	= ccu_fixed_factor_recalc_rate,
	.round_rate	= ccu_fixed_factor_round_rate,
	.set_rate	= ccu_fixed_factor_set_rate,
};
