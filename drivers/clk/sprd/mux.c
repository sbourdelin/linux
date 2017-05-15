/*
 * Spreadtrum multiplexer clock driver
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

void __init sprd_mux_clk_setup(struct device_node *node)
{
	struct clk *clk;
	const char *clk_name = node->name;
	const char **parent_names;
	int   num_parents = 0;
	void __iomem *reg;
	u32  msk = 0;
	u8   shift = 0;
	u8   width = 0;
	int i = 0;

	if (of_property_read_string(node, "clock-output-names", &clk_name))
		return;

	if (of_property_read_u32(node, "sprd,mux-msk", &msk)) {
		pr_err("%s: No mux-msk property found for %s!\n",
		       __func__, clk_name);
		return;
	}
	shift = __ffs(msk);
	width = fls(msk) - shift;

	num_parents = of_clk_get_parent_count(node);
	if (!num_parents) {
		pr_err("%s: no parent found for %s!\n",
		       __func__, clk_name);
		return;
	}

	parent_names = kcalloc(num_parents, sizeof(char *), GFP_KERNEL);
	if (!parent_names)
		return;

	while (i < num_parents &&
		(parent_names[i] =
		 of_clk_get_parent_name(node, i)) != NULL)
		i++;

	reg = of_iomap(node, 0);
	if (!reg) {
		pr_err("%s: mux-clock[%s] of_iomap failed!\n", __func__,
			clk_name);
		goto kfree_parent_names;
	}

	clk = clk_register_mux(NULL, clk_name, parent_names, num_parents,
			       CLK_SET_RATE_NO_REPARENT | CLK_GET_RATE_NOCACHE,
			       reg, shift, width, 0, NULL);
	if (clk) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
		return;
	}

	iounmap(reg);

kfree_parent_names:
	kfree(parent_names);
}

CLK_OF_DECLARE(muxed_clock, "sprd,muxed-clock", sprd_mux_clk_setup);
