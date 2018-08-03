/* SPDX-License-Identifier: GPL-2.0 */
/*
 *  Copyright(c) 2018 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 */

#ifndef __INTEL_CLK_PLL_H
#define __INTEL_CLK_PLL_H

enum intel_pll_type {
	pll_grx500,
};

struct intel_pll_rate_table {
	unsigned long	prate;
	unsigned long	rate;
	unsigned int	mult;
	unsigned int	div;
	unsigned int	frac;
};

struct intel_clk_pll {
	struct clk_hw	hw;
	struct regmap	*map;
	unsigned int	reg;
	unsigned long	flags;
	unsigned int	mult;
	unsigned int	div;
	unsigned int	frac;
	unsigned int	table_sz;
	const struct intel_pll_rate_table *rate_table;
};

#endif /* __INTEL_CLK_PLL_H */
