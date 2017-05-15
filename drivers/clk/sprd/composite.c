/*
 * Spreadtrum composite clock driver
 *
 * Copyright (C) 2015~2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>

void __init sprd_composite_clk_setup(struct device_node *node)
{
	struct clk *clk;
	struct clk_mux *mux = NULL;
	const struct clk_ops *mux_ops = NULL;
	struct clk_divider *div = NULL;
	const struct clk_ops *div_ops = NULL;
	const char *clk_name = node->name;
	const char **parent_names;
	unsigned long flags = 0;
	u32 msk;
	int num_parents = 0;
	int i = 0;
	int index = 0;

	if (of_property_read_string(node, "clock-output-names", &clk_name))
		return;

	num_parents = of_clk_get_parent_count(node);
	if (!num_parents) {
		pr_err("%s: Failed to get %s's parent number!\n",
		       __func__, clk_name);
		return;
	}

	parent_names = kzalloc((sizeof(char *) * num_parents), GFP_KERNEL);
	if (!parent_names)
		return;

	while (i < num_parents &&
			(parent_names[i] =
			 of_clk_get_parent_name(node, i)) != NULL)
		i++;

	mux = kzalloc(sizeof(struct clk_mux), GFP_KERNEL);
	if (!mux)
		goto kfree_parent_names;

	if (!of_property_read_u32(node, "sprd,mux-msk", &msk)) {
		mux->reg = of_iomap(node, index++);
		if (!mux->reg)
			goto kfree_mux;

		mux->shift = __ffs(msk);
		mux->mask = msk >> (mux->shift);
		mux_ops = &clk_mux_ops;
	}

	div = kzalloc(sizeof(struct clk_divider), GFP_KERNEL);
	if (!div)
		goto iounmap_mux_reg;

	if (!of_property_read_u32(node, "sprd,div-msk", &msk)) {
		div->reg = of_iomap(node, index);
		if (!div->reg)
			div->reg = mux->reg;
		if (!div->reg)
			goto iounmap_mux_reg;

		div->shift = __ffs(msk);
		div->width = fls(msk) - div->shift;
		div_ops = &clk_divider_ops;
	}

	flags |= CLK_IGNORE_UNUSED;
	clk = clk_register_composite(NULL, clk_name, parent_names, num_parents,
				     &mux->hw, mux_ops, &div->hw, div_ops, NULL,
				     NULL, flags);
	if (!IS_ERR(clk)) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
		return;
	}

	if (div->reg)
		if (!mux || (div->reg != mux->reg))
			iounmap(div->reg);

	kfree(div);

iounmap_mux_reg:
	if (mux->reg)
		iounmap(mux->reg);

kfree_mux:
	kfree(mux);

kfree_parent_names:
	pr_err("Failed to register composite clk %s!\n", clk_name);
	kfree(parent_names);
}

CLK_OF_DECLARE(composite_clock, "sprd,composite-clock",
	       sprd_composite_clk_setup);
