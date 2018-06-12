/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __INTEL_CLK_API_H
#define __INTEL_CLK_API_H

/*
 *  Copyright(c) 2016 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 *
 */

struct div_clk_data {
	u8 shift;
	u8 width;
	const unsigned int tbl_sz;
	const struct clk_div_table *div_table;
	unsigned long flags;
};

struct mux_clk_data {
	u8 shift;
	u8 width;
	const u32 *table;
	unsigned long flags;
	unsigned long clk_flags;
};

struct gate_clk_data {
	unsigned long mask;
	unsigned long def_onoff;
	u8 reg_size;
	unsigned long flags;
};

struct gate_dummy_clk_data {
	unsigned int def_val;
	unsigned long flags;
};

struct fixed_rate_clk_data {
	u8 shift;
	u8 width;
	unsigned long fixed_rate;
	unsigned int setval;
};

struct gate_dummy_clk {
	struct clk_hw hw;
	unsigned int clk_status;
};

struct div_clk {
	struct clk_hw hw;
	struct regmap *map;
	unsigned int reg;
	u8 shift;
	u8 width;
	unsigned int flags;
	const struct clk_div_table *div_table;
	unsigned int tbl_sz;
};

struct gate_clk {
	struct clk_hw hw;
	struct regmap *map;
	unsigned int reg;
	u8 bit_idx;
	unsigned int flags;
};

struct mux_clk {
	struct clk_hw hw;
	struct regmap *map;
	unsigned int reg;
	const u32 *table;
	u8 shift;
	u8 width;
	unsigned int flags;
};

/**
 * struct clk_fixed_factor_frac - fixed multiplier/divider/fraction clock
 *
 * @hw:		handle between common and hardware-specific interfaces
 * @mult:	multiplier(N)
 * @div:	divider(M)
 * @frac:	fraction(K)
 * @frac_div:	fraction divider(D)
 *
 * Clock with a fixed multiplier, divider and fraction.
 * The output frequency formula is clk = parent clk * (N+K/D)/M.
 * Implements .recalc_rate, .set_rate and .round_rate
 */

struct clk_fixed_factor_frac {
	struct clk_hw	hw;
	unsigned int	mult;
	unsigned int	div;
	unsigned int	frac;
	unsigned int	frac_div;
};

#define INTEL_FIXED_FACTOR_PLLCLK	"intel,fixed-factor-pllclk"
#define INTEL_FIXED_FACTOR_FRAC_PLLCLK	"intel,fixed-factor-frac-pllclk"

#define CLK_INIT_DEF_CFG_REQ		BIT(0)

void intel_gate_clk_setup(struct device_node *np,
			  const struct gate_clk_data *data);
void intel_mux_clk_setup(struct device_node *np,
			 const struct mux_clk_data *data);
void intel_fixed_rate_clk_setup(struct device_node *np,
				const struct fixed_rate_clk_data *data);
void intel_div_clk_setup(struct device_node *np,
			 const struct div_clk_data *data);
void intel_gate_dummy_clk_setup(struct device_node *np,
				const struct gate_dummy_clk_data *data);
void intel_cluster_div_clk_setup(struct device_node *np,
				 const struct div_clk_data *data, u32 num);

#endif /* __INTEL_CLK_API_H */
