/*
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __QCOM_CLK_ALPHA_PLL_H__
#define __QCOM_CLK_ALPHA_PLL_H__

#include <linux/clk-provider.h>
#include "clk-regmap.h"

enum {
	ALPHA_PLL_MODE,
	ALPHA_PLL_L_VAL,
	ALPHA_PLL_ALPHA_VAL,
	ALPHA_PLL_ALPHA_VAL_U,
	ALPHA_PLL_USER_CTL,
	ALPHA_PLL_USER_CTL_U,
	ALPHA_PLL_CONFIG_CTL,
	ALPHA_PLL_CONFIG_CTL_U,
	ALPHA_PLL_TEST_CTL,
	ALPHA_PLL_TEST_CTL_U,
	ALPHA_PLL_STATUS,
};

struct pll_vco {
	unsigned long min_freq;
	unsigned long max_freq;
	u32 val;
};

/**
 * struct clk_alpha_pll - phase locked loop (PLL)
 * @base: base address of registers
 * @offsets: array containing offsets of all PLL registers from base address
 * @vco_table: array of VCO settings
 * @clkr: regmap clock handle
 */
struct clk_alpha_pll {
	u32 base;
	const u8 *offsets;

	const struct pll_vco *vco_table;
	size_t num_vco;
#define SUPPORTS_OFFLINE_REQ		BIT(0)
#define SUPPORTS_16BIT_ALPHA		BIT(1)
#define SUPPORTS_FSM_MODE		BIT(2)
#define SUPPORTS_64BIT_CONFIG_CTL	BIT(3)
#define SUPPORTS_DYNAMIC_UPDATE		BIT(4)
#define HAVE_NO_VCO_CONF		BIT(5)
	u8 flags;

	struct clk_regmap clkr;
};

#define CLK_HUAYRA_PLL_FLAGS	(HAVE_NO_VCO_CONF | SUPPORTS_DYNAMIC_UPDATE | \
				 SUPPORTS_64BIT_CONFIG_CTL |		      \
				 SUPPORTS_16BIT_ALPHA)
#define CLK_BRAMMO_PLL_FLAGS	(HAVE_NO_VCO_CONF | SUPPORTS_DYNAMIC_UPDATE)

/**
 * struct clk_alpha_pll_postdiv - phase locked loop (PLL) post-divider
 * @base: base address of registers
 * @offsets: array containing offsets of all PLL registers from base address
 * @width: width of post-divider
 * @clkr: regmap clock handle
 */
struct clk_alpha_pll_postdiv {
	u32 base;
	const u8 *offsets;
	u8 width;

	struct clk_regmap clkr;
};

struct alpha_pll_config {
	u32 l;
	u32 alpha;
	u32 alpha_hi;
	u32 config_ctl_val;
	u32 config_ctl_hi_val;
	u32 main_output_mask;
	u32 aux_output_mask;
	u32 aux2_output_mask;
	u32 early_output_mask;
	u32 alpha_en_mask;
	u32 alpha_mode_mask;
	u32 pre_div_val;
	u32 pre_div_mask;
	u32 post_div_val;
	u32 post_div_mask;
	u32 vco_val;
	u32 vco_mask;
};

extern const u8 alpha_pll_offsets[];
extern const u8 huayra_pll_offsets[];
extern const u8 brammo_pll_offsets[];

extern const struct clk_ops clk_alpha_pll_ops;
extern const struct clk_ops clk_alpha_pll_hwfsm_ops;
extern const struct clk_ops clk_alpha_pll_huayra_ops;
extern const struct clk_ops clk_alpha_pll_postdiv_ops;

void clk_alpha_pll_configure(struct clk_alpha_pll *pll, struct regmap *regmap,
			     const struct alpha_pll_config *config);

#endif
