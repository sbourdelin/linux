/*
 * Spreadtrum divider clock driver
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

void __init sprd_divider_clk_setup(struct device_node *node)
{
	struct clk *clk, *pclk;
	struct clk_divider *clk_div;
	struct clk_composite *clk_composite;
	const char *clk_name = node->name;
	const char *parent;
	void __iomem *reg;
	u32 msk = 0;
	u8 shift = 0;
	u8 width = 0;

	if (of_property_read_string(node, "clock-output-names", &clk_name))
		return;

	parent = of_clk_get_parent_name(node, 0);

	if (of_property_read_bool(node, "reg")) {
		reg = of_iomap(node, 0);
	} else {
		pclk = __clk_lookup(parent);
		if (!pclk) {
			pr_err("%s: clock[%s] has no reg and parent!\n",
			       __func__, clk_name);
			return;
		}

		clk_composite = container_of(__clk_get_hw(pclk),
					     struct clk_composite, hw);

		clk_div = container_of(clk_composite->rate_hw,
				       struct clk_divider, hw);

		reg = clk_div->reg;
	}

	if (!reg) {
		pr_err("%s: clock[%s] remap register failed!\n",
		       __func__, clk_name);
		return;
	}

	if (of_property_read_u32(node, "sprd,div-msk", &msk)) {
		pr_err("%s: Failed to get %s's div-msk\n", __func__, clk_name);
		goto iounmap_reg;
	}

	shift = __ffs(msk);
	width = fls(msk) - shift;
	clk = clk_register_divider(NULL, clk_name, parent,
				   0, reg, shift, width, 0, NULL);
	if (!IS_ERR(clk)) {
		of_clk_add_provider(node, of_clk_src_simple_get, clk);
		clk_register_clkdev(clk, clk_name, NULL);
		return;
	}

iounmap_reg:
	iounmap(reg);
	pr_err("%s: Failed to register divider clock[%s]!\n",
	       __func__, clk_name);
}

CLK_OF_DECLARE(divider_clock, "sprd,divider-clock", sprd_divider_clk_setup);
