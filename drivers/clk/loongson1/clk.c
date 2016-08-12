/*
 * Copyright (c) 2012-2016 Zhang, Keguang <keguang.zhang@gmail.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/clk-provider.h>
#include <linux/slab.h>

int ls1x_pll_clk_enable(struct clk_hw *hw)
{
	return 0;
}

void ls1x_pll_clk_disable(struct clk_hw *hw)
{
}

struct clk *__init clk_register_pll(struct device *dev,
				    const char *name,
				    const char *parent_name,
				    const struct clk_ops *ops,
				    unsigned long flags)
{
	struct clk_hw *hw;
	struct clk *clk;
	struct clk_init_data init;

	/* allocate the divider */
	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return ERR_PTR(-ENOMEM);

	init.name = name;
	init.ops = ops;
	init.flags = flags | CLK_IS_BASIC;
	init.parent_names = (parent_name ? &parent_name : NULL);
	init.num_parents = (parent_name ? 1 : 0);
	hw->init = &init;

	/* register the clock */
	clk = clk_register(dev, hw);

	if (IS_ERR(clk))
		kfree(hw);

	return clk;
}

