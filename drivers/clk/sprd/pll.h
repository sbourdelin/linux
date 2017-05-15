/*
 * Spreatrum clock pll driver head file
 *
 * Copyright (C) 2015~2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef __SPRD_PLL_H__
#define __SPRD_PLL_H__

#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/of.h>
#include <linux/of_address.h>

#define SPRD_PLL_MAX_RATE	ULONG_MAX
#define SPRD_PLL_MAX_REGNUM	(3)
#define SPRD_DELAY_200		(200)
#define SPRD_DELAY_1000		(1000)

struct reg_cfg {
	u32 val;
	u32 msk;
};

struct pll_common {
	u32 value;
	u8  index;
};

struct fvco_threshold {
	unsigned long rate;
	int flag;
};

struct pll_div_mask {
	u32 value;
	u8  index;
	struct fvco_threshold *fvco_threshold;
};

struct pll_ibias_table {
	unsigned long rate;
	u8 ibias;
};

struct sprd_pll_config {
	char	*name;
	u32		udelay;
	struct pll_common	lock_done;
	struct pll_common	div_s;
	struct pll_common	mod_en;
	struct pll_common	sdm_en;
	struct pll_common	refin_msk;
	struct pll_common	ibias_msk;
	struct pll_common	pll_n_msk;
	struct pll_common	nint_msk;
	struct pll_common	kint_msk;
	struct pll_div_mask	prediv_msk;
	struct pll_div_mask	postdiv_msk;
	struct pll_ibias_table	*itable;
};

struct sprd_pll_hw {
	struct clk_hw	hw;
	void __iomem	*reg[SPRD_PLL_MAX_REGNUM];
	int		reg_num;
};

#define to_sprd_pll_hw(_hw) container_of(_hw, struct sprd_pll_hw, hw)

#endif
