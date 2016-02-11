/*
 * ARTPEC-6 clock initialization
 *
 * Copyright 2015 Axis Comunications AB.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>

static void __init of_artpec6_pll1_setup(struct device_node *np)
{
	void __iomem *devstat;
	struct clk *clk;
	const char *clk_name = np->name;
	const char *parent_name;
	u32 pll_mode, pll_m, pll_n;

	parent_name = of_clk_get_parent_name(np, 0);

	devstat = of_iomap(np, 0);
	if (devstat == NULL) {
		pr_err("error to ioremap DEVSTAT\n");
		return;
	}

	/* DEVSTAT register contains PLL settings */
	pll_mode = (readl(devstat) >> 6) & 3;
	iounmap(devstat);

	/*
	 * pll1 settings are designed for different DDR speeds using a fixed
	 * 50MHz external clock. However, a different external clock could be
	 * used on different boards.
	 * CPU clock is half the DDR clock.
	 */
	switch (pll_mode) {
	case 0: /* DDR3-2133 mode */
		pll_m = 4;
		pll_n = 85;
		break;
	case 1: /* DDR3-1866 mode */
		pll_m = 6;
		pll_n = 112;
		break;
	case 2: /* DDR3-1600 mode */
		pll_m = 4;
		pll_n = 64;
		break;
	case 3: /* DDR3-1333 mode */
		pll_m = 8;
		pll_n = 106;
		break;
	}
	/* ext_clk is defined in device tree */
	clk = clk_register_fixed_factor(NULL, clk_name, parent_name, 0,
			pll_n, pll_m);
	if (IS_ERR(clk)) {
		pr_err("%s not registered\n", clk_name);
		return;
	}
	of_clk_add_provider(np, of_clk_src_simple_get, clk);
}
CLK_OF_DECLARE(artpec6_pll1, "axis,artpec6-pll1-clock", of_artpec6_pll1_setup);
