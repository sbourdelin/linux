/*
 * Copyright 2016 Maxime Ripard
 *
 * Maxime Ripard <maxime.ripard@free-electrons.com>
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

#include <linux/clk-provider.h>
#include <linux/iopoll.h>
#include <linux/slab.h>

#include "ccu_common.h"
#include "ccu_reset.h"

static DEFINE_SPINLOCK(ccu_lock);

void ccu_helper_wait_for_lock(struct ccu_common *common, u32 lock)
{
	u32 reg;

	if (!(common->features & CCU_FEATURE_PLL_LOCK))
		return;

	WARN_ON(readl_relaxed_poll_timeout(common->base + common->reg, reg,
					   !(reg & lock), 100, 70000));
}

int sunxi_ccu_probe(struct device_node *node, void __iomem *reg,
		    const struct sunxi_ccu_desc *desc)
{
	struct ccu_common **cclks = desc->clks;
	size_t num_clks = desc->num_clks;
	struct clk_onecell_data *data;
	struct ccu_reset *reset;
	struct clk **clks;
	int i, ret;

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	clks = kcalloc(num_clks, sizeof(struct clk *), GFP_KERNEL);
	if (!clks)
		return -ENOMEM;

	data->clks = clks;
	data->clk_num = num_clks;

	for (i = 0; i < num_clks; i++) {
		struct ccu_common *cclk = cclks[i];
		struct clk *clk;

		if (!cclk) {
			clks[i] = ERR_PTR(-ENOENT);
			continue;
		}

		cclk->base = reg;
		cclk->lock = &ccu_lock;

		clk = clk_register(NULL, &cclk->hw);
		if (IS_ERR(clk))
			continue;

		clks[i] = clk;
	}

	ret = of_clk_add_provider(node, of_clk_src_onecell_get, data);
	if (ret)
		goto err_clk_unreg;

	reset = kzalloc(sizeof(*reset), GFP_KERNEL);
	reset->rcdev.of_node = node;
	reset->rcdev.ops = &ccu_reset_ops;
	reset->rcdev.owner = THIS_MODULE;
	reset->rcdev.nr_resets = desc->num_resets;
	reset->base = reg;
	reset->lock = &ccu_lock;
	reset->reset_map = desc->resets;

	ret = reset_controller_register(&reset->rcdev);
	if (ret)
		goto err_of_clk_unreg;

	return 0;

err_of_clk_unreg:
err_clk_unreg:
	return ret;
}
