// SPDX-License-Identifier: GPL-2.0+
/*
 * Amlogic Meson-G12A Clock Controller Driver
 *
 * Copyright (c) 2016 Baylibre SAS.
 * Author: Michael Turquette <mturquette@baylibre.com>
 *
 * Copyright (c) 2018 Amlogic, inc.
 * Author: Qiufang Dai <qiufang.dai@amlogic.com>
 * Author: Jian Hu <jian.hu@amlogic.com>
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/mfd/syscon.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "clkc.h"
#include "g12a.h"

static DEFINE_SPINLOCK(meson_clk_lock);

static struct clk_regmap g12a_fixed_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = HHI_FIX_PLL_CNTL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = HHI_FIX_PLL_CNTL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = HHI_FIX_PLL_CNTL0,
			.shift   = 10,
			.width   = 5,
		},
		.frac = {
			.reg_off = HHI_FIX_PLL_CNTL1,
			.shift   = 0,
			.width   = 19,
		},
		.l = {
			.reg_off = HHI_FIX_PLL_CNTL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = HHI_FIX_PLL_CNTL0,
			.shift   = 29,
			.width   = 1,
		},
	},
	.hw.init = &(struct clk_init_data){
		.name = "fixed_pll_dco",
		.ops = &meson_clk_pll_ro_ops,
		.parent_names = (const char *[]){ "g12a_ee_core" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_fixed_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = HHI_FIX_PLL_CNTL0,
		.shift = 16,
		.width = 2,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fixed_pll",
		.ops = &clk_regmap_divider_ro_ops,
		.parent_names = (const char *[]){ "fixed_pll_dco" },
		.num_parents = 1,
		/*
		 * This clock won't ever change at runtime so
		 * CLK_SET_RATE_PARENT is not required
		 */
	},
};

/*
 * Internal sys pll emulation configuration parameters
 */
static const struct reg_sequence g12a_sys_init_regs[] = {
	{ .reg = HHI_SYS_PLL_CNTL1,	.def = 0x00000000 },
	{ .reg = HHI_SYS_PLL_CNTL2,	.def = 0x00000000 },
	{ .reg = HHI_SYS_PLL_CNTL3,	.def = 0x48681c00 },
	{ .reg = HHI_SYS_PLL_CNTL4,	.def = 0x88770290 },
	{ .reg = HHI_SYS_PLL_CNTL5,	.def = 0x39272000 },
	{ .reg = HHI_SYS_PLL_CNTL6,	.def = 0x56540000 },
};

static struct clk_regmap g12a_sys_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = HHI_SYS_PLL_CNTL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = HHI_SYS_PLL_CNTL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = HHI_SYS_PLL_CNTL0,
			.shift   = 10,
			.width   = 5,
		},
		.l = {
			.reg_off = HHI_SYS_PLL_CNTL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = HHI_SYS_PLL_CNTL0,
			.shift   = 29,
			.width   = 1,
		},
		.init_regs = g12a_sys_init_regs,
		.init_count = ARRAY_SIZE(g12a_sys_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "sys_pll_dco",
		.ops = &meson_clk_pll_ro_ops,
		.parent_names = (const char *[]){ "g12a_ee_core" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_sys_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = HHI_SYS_PLL_CNTL0,
		.shift = 16,
		.width = 3,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sys_pll",
		.ops = &clk_regmap_divider_ro_ops,
		.parent_names = (const char *[]){ "sys_pll_dco" },
		.num_parents = 1,
	},
};

static const struct pll_params_table g12a_gp0_pll_params_table[] = {
	PLL_PARAMS(40, 1),
	PLL_PARAMS(41, 1),
	PLL_PARAMS(42, 1),
	PLL_PARAMS(43, 1),
	PLL_PARAMS(44, 1),
	PLL_PARAMS(45, 1),
	PLL_PARAMS(46, 1),
	PLL_PARAMS(47, 1),
	PLL_PARAMS(48, 1),
	PLL_PARAMS(49, 1),
	PLL_PARAMS(50, 1),
	PLL_PARAMS(51, 1),
	PLL_PARAMS(52, 1),
	PLL_PARAMS(53, 1),
	PLL_PARAMS(54, 1),
	PLL_PARAMS(55, 1),
	PLL_PARAMS(56, 1),
	PLL_PARAMS(57, 1),
	PLL_PARAMS(58, 1),
	PLL_PARAMS(59, 1),
	PLL_PARAMS(60, 1),
	PLL_PARAMS(61, 1),
	PLL_PARAMS(62, 1),
	PLL_PARAMS(63, 1),
	PLL_PARAMS(64, 1),
	PLL_PARAMS(65, 1),
	PLL_PARAMS(66, 1),
	PLL_PARAMS(67, 1),
	PLL_PARAMS(68, 1),
	{ /* sentinel */ },
};

/*
 * Internal gp0 pll emulation configuration parameters
 */
static const struct reg_sequence g12a_gp0_init_regs[] = {
	{ .reg = HHI_GP0_PLL_CNTL1,	.def = 0x00000000 },
	{ .reg = HHI_GP0_PLL_CNTL2,	.def = 0x00000000 },
	{ .reg = HHI_GP0_PLL_CNTL3,	.def = 0x48681c00 },
	{ .reg = HHI_GP0_PLL_CNTL4,	.def = 0x33771290 },
	{ .reg = HHI_GP0_PLL_CNTL5,	.def = 0x39272000 },
	{ .reg = HHI_GP0_PLL_CNTL6,	.def = 0x56540000 },
};

static struct clk_regmap g12a_gp0_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = HHI_GP0_PLL_CNTL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = HHI_GP0_PLL_CNTL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = HHI_GP0_PLL_CNTL0,
			.shift   = 10,
			.width   = 5,
		},
		.frac = {
			.reg_off = HHI_GP0_PLL_CNTL1,
			.shift   = 0,
			.width   = 19,
		},
		.l = {
			.reg_off = HHI_GP0_PLL_CNTL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = HHI_GP0_PLL_CNTL0,
			.shift   = 29,
			.width   = 1,
		},
		.table = g12a_gp0_pll_params_table,
		.init_regs = g12a_gp0_init_regs,
		.init_count = ARRAY_SIZE(g12a_gp0_init_regs),
	},
	.hw.init = &(struct clk_init_data){
		.name = "gp0_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_names = (const char *[]){ "g12a_ee_core" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_gp0_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = HHI_GP0_PLL_CNTL0,
		.shift = 16,
		.width = 3,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "gp0_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]){ "gp0_pll_dco" },
		.num_parents = 1,
	},
};

/*
 * Internal hifi pll emulation configuration parameters
 */
static const struct reg_sequence g12a_hifi_init_regs[] = {
	{ .reg = HHI_HIFI_PLL_CNTL1,	.def = 0x00000000 },
	{ .reg = HHI_HIFI_PLL_CNTL2,	.def = 0x00000000 },
	{ .reg = HHI_HIFI_PLL_CNTL3,	.def = 0x6a285c00 },
	{ .reg = HHI_HIFI_PLL_CNTL4,	.def = 0x65771290 },
	{ .reg = HHI_HIFI_PLL_CNTL5,	.def = 0x39272000 },
	{ .reg = HHI_HIFI_PLL_CNTL6,	.def = 0x56540000 },
};

static struct clk_regmap g12a_hifi_pll_dco = {
	.data = &(struct meson_clk_pll_data){
		.en = {
			.reg_off = HHI_HIFI_PLL_CNTL0,
			.shift   = 28,
			.width   = 1,
		},
		.m = {
			.reg_off = HHI_HIFI_PLL_CNTL0,
			.shift   = 0,
			.width   = 8,
		},
		.n = {
			.reg_off = HHI_HIFI_PLL_CNTL0,
			.shift   = 10,
			.width   = 5,
		},
		.frac = {
			.reg_off = HHI_HIFI_PLL_CNTL1,
			.shift   = 0,
			.width   = 19,
		},
		.l = {
			.reg_off = HHI_HIFI_PLL_CNTL0,
			.shift   = 31,
			.width   = 1,
		},
		.rst = {
			.reg_off = HHI_HIFI_PLL_CNTL0,
			.shift   = 29,
			.width   = 1,
		},
		.table = g12a_gp0_pll_params_table,
		.init_regs = g12a_hifi_init_regs,
		.init_count = ARRAY_SIZE(g12a_hifi_init_regs),
		.flags = CLK_MESON_PLL_ROUND_CLOSEST,
	},
	.hw.init = &(struct clk_init_data){
		.name = "hifi_pll_dco",
		.ops = &meson_clk_pll_ops,
		.parent_names = (const char *[]){ "g12a_ee_core" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_hifi_pll = {
	.data = &(struct clk_regmap_div_data){
		.offset = HHI_HIFI_PLL_CNTL0,
		.shift = 16,
		.width = 2,
		.flags = CLK_DIVIDER_POWER_OF_TWO,
	},
	.hw.init = &(struct clk_init_data){
		.name = "hifi_pll",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]){ "hifi_pll_dco" },
		.num_parents = 1,
	},
};

static struct clk_fixed_factor g12a_fclk_div2_div = {
	.mult = 1,
	.div = 2,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div2_div",
		.ops = &clk_fixed_factor_ops,
		.parent_names = (const char *[]){ "fixed_pll" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_fclk_div2 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_FIX_PLL_CNTL1,
		.bit_idx = 24,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div2",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "fclk_div2_div" },
		.num_parents = 1,
	},
};

static struct clk_fixed_factor g12a_fclk_div3_div = {
	.mult = 1,
	.div = 3,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div3_div",
		.ops = &clk_fixed_factor_ops,
		.parent_names = (const char *[]){ "fixed_pll" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_fclk_div3 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_FIX_PLL_CNTL1,
		.bit_idx = 20,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div3",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "fclk_div3_div" },
		.num_parents = 1,
	},
};

static struct clk_fixed_factor g12a_fclk_div4_div = {
	.mult = 1,
	.div = 4,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div4_div",
		.ops = &clk_fixed_factor_ops,
		.parent_names = (const char *[]){ "fixed_pll" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_fclk_div4 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_FIX_PLL_CNTL1,
		.bit_idx = 21,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div4",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "fclk_div4_div" },
		.num_parents = 1,
	},
};

static struct clk_fixed_factor g12a_fclk_div5_div = {
	.mult = 1,
	.div = 5,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div5_div",
		.ops = &clk_fixed_factor_ops,
		.parent_names = (const char *[]){ "fixed_pll" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_fclk_div5 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_FIX_PLL_CNTL1,
		.bit_idx = 22,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div5",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "fclk_div5_div" },
		.num_parents = 1,
	},
};

static struct clk_fixed_factor g12a_fclk_div7_div = {
	.mult = 1,
	.div = 7,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div7_div",
		.ops = &clk_fixed_factor_ops,
		.parent_names = (const char *[]){ "fixed_pll" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_fclk_div7 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_FIX_PLL_CNTL1,
		.bit_idx = 23,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div7",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "fclk_div7_div" },
		.num_parents = 1,
	},
};

static struct clk_fixed_factor g12a_fclk_div2p5_div = {
	.mult = 2,
	.div = 5,
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div2p5_div",
		.ops = &clk_fixed_factor_ops,
		.parent_names = (const char *[]){ "fixed_pll" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_fclk_div2p5 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_FIX_PLL_CNTL1,
		.bit_idx = 25,
	},
	.hw.init = &(struct clk_init_data){
		.name = "fclk_div2p5",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "fclk_div2p5_div" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_mpll0_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = HHI_MPLL_CNTL1,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = HHI_MPLL_CNTL1,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = HHI_MPLL_CNTL1,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = HHI_MPLL_CNTL1,
			.shift   = 29,
			.width	 = 1,
		},
		.lock = &meson_clk_lock,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll0_div",
		.ops = &meson_clk_mpll_ops,
		.parent_names = (const char *[]){ "fixed_pll_dco" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_mpll0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_MPLL_CNTL1,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll0",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "mpll0_div" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap g12a_mpll1_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = HHI_MPLL_CNTL3,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = HHI_MPLL_CNTL3,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = HHI_MPLL_CNTL3,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = HHI_MPLL_CNTL3,
			.shift   = 29,
			.width	 = 1,
		},
		.lock = &meson_clk_lock,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll1_div",
		.ops = &meson_clk_mpll_ops,
		.parent_names = (const char *[]){ "fixed_pll_dco" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_mpll1 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_MPLL_CNTL3,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll1",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "mpll1_div" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap g12a_mpll2_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = HHI_MPLL_CNTL5,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = HHI_MPLL_CNTL5,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = HHI_MPLL_CNTL5,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = HHI_MPLL_CNTL5,
			.shift   = 29,
			.width	 = 1,
		},
		.lock = &meson_clk_lock,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll2_div",
		.ops = &meson_clk_mpll_ops,
		.parent_names = (const char *[]){ "fixed_pll_dco" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_mpll2 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_MPLL_CNTL5,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll2",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "mpll2_div" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap g12a_mpll3_div = {
	.data = &(struct meson_clk_mpll_data){
		.sdm = {
			.reg_off = HHI_MPLL_CNTL7,
			.shift   = 0,
			.width   = 14,
		},
		.sdm_en = {
			.reg_off = HHI_MPLL_CNTL7,
			.shift   = 30,
			.width	 = 1,
		},
		.n2 = {
			.reg_off = HHI_MPLL_CNTL7,
			.shift   = 20,
			.width   = 9,
		},
		.ssen = {
			.reg_off = HHI_MPLL_CNTL7,
			.shift   = 29,
			.width	 = 1,
		},
		.lock = &meson_clk_lock,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll3_div",
		.ops = &meson_clk_mpll_ops,
		.parent_names = (const char *[]){ "fixed_pll_dco" },
		.num_parents = 1,
	},
};

static struct clk_regmap g12a_mpll3 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_MPLL_CNTL7,
		.bit_idx = 31,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpll3",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "mpll3_div" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static u32 mux_table_clk81[]	= { 0, 2, 3, 4, 5, 6, 7 };
static const char * const clk81_parent_names[] = {
	"g12a_ee_core", "fclk_div7", "mpll1", "mpll2", "fclk_div4",
	"fclk_div3", "fclk_div5"
};

static struct clk_regmap g12a_mpeg_clk_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = HHI_MPEG_CLK_CNTL,
		.mask = 0x7,
		.shift = 12,
		.table = mux_table_clk81,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpeg_clk_sel",
		.ops = &clk_regmap_mux_ro_ops,
		.parent_names = clk81_parent_names,
		.num_parents = ARRAY_SIZE(clk81_parent_names),
	},
};

static struct clk_regmap g12a_mpeg_clk_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = HHI_MPEG_CLK_CNTL,
		.shift = 0,
		.width = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "mpeg_clk_div",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]){ "mpeg_clk_sel" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap g12a_clk81 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_MPEG_CLK_CNTL,
		.bit_idx = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "clk81",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "mpeg_clk_div" },
		.num_parents = 1,
		.flags = (CLK_SET_RATE_PARENT | CLK_IS_CRITICAL),
	},
};

static const char * const g12a_sd_emmc_clk0_parent_names[] = {
	"g12a_ee_core", "fclk_div2", "fclk_div3", "fclk_div5", "fclk_div7",

	/*
	 * Following these parent clocks, we should also have had mpll2, mpll3
	 * and gp0_pll but these clocks are too precious to be used here. All
	 * the necessary rates for MMC and NAND operation can be acheived using
	 * g12a_ee_core or fclk_div clocks
	 */
};

/* SDcard clock */
static struct clk_regmap g12a_sd_emmc_b_clk0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = HHI_SD_EMMC_CLK_CNTL,
		.mask = 0x7,
		.shift = 25,
		.flags = CLK_MUX_ROUND_CLOSEST,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_b_clk0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_names = g12a_sd_emmc_clk0_parent_names,
		.num_parents = ARRAY_SIZE(g12a_sd_emmc_clk0_parent_names),
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap g12a_sd_emmc_b_clk0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = HHI_SD_EMMC_CLK_CNTL,
		.shift = 16,
		.width = 7,
		.flags = CLK_DIVIDER_ROUND_CLOSEST,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_b_clk0_div",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]){ "sd_emmc_b_clk0_sel" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap g12a_sd_emmc_b_clk0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_SD_EMMC_CLK_CNTL,
		.bit_idx = 23,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sd_emmc_b_clk0",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "sd_emmc_b_clk0_div" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/* EMMC/NAND clock */
static struct clk_regmap g12a_sd_emmc_c_clk0_sel = {
	.data = &(struct clk_regmap_mux_data){
		.offset = HHI_NAND_CLK_CNTL,
		.mask = 0x7,
		.shift = 9,
		.flags = CLK_MUX_ROUND_CLOSEST,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_c_clk0_sel",
		.ops = &clk_regmap_mux_ops,
		.parent_names = g12a_sd_emmc_clk0_parent_names,
		.num_parents = ARRAY_SIZE(g12a_sd_emmc_clk0_parent_names),
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap g12a_sd_emmc_c_clk0_div = {
	.data = &(struct clk_regmap_div_data){
		.offset = HHI_NAND_CLK_CNTL,
		.shift = 0,
		.width = 7,
		.flags = CLK_DIVIDER_ROUND_CLOSEST,
	},
	.hw.init = &(struct clk_init_data) {
		.name = "sd_emmc_c_clk0_div",
		.ops = &clk_regmap_divider_ops,
		.parent_names = (const char *[]){ "sd_emmc_c_clk0_sel" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

static struct clk_regmap g12a_sd_emmc_c_clk0 = {
	.data = &(struct clk_regmap_gate_data){
		.offset = HHI_NAND_CLK_CNTL,
		.bit_idx = 7,
	},
	.hw.init = &(struct clk_init_data){
		.name = "sd_emmc_c_clk0",
		.ops = &clk_regmap_gate_ops,
		.parent_names = (const char *[]){ "sd_emmc_c_clk0_div" },
		.num_parents = 1,
		.flags = CLK_SET_RATE_PARENT,
	},
};

/* Everything Else (EE) domain gates */
static MESON_GATE(g12a_ddr,			HHI_GCLK_MPEG0,	0);
static MESON_GATE(g12a_dos,			HHI_GCLK_MPEG0,	1);
static MESON_GATE(g12a_audio_locker,		HHI_GCLK_MPEG0,	2);
static MESON_GATE(g12a_mipi_dsi_host,		HHI_GCLK_MPEG0,	3);
static MESON_GATE(g12a_eth_phy,			HHI_GCLK_MPEG0,	4);
static MESON_GATE(g12a_isa,			HHI_GCLK_MPEG0,	5);
static MESON_GATE(g12a_pl301,			HHI_GCLK_MPEG0,	6);
static MESON_GATE(g12a_periphs,			HHI_GCLK_MPEG0,	7);
static MESON_GATE(g12a_spicc_0,			HHI_GCLK_MPEG0,	8);
static MESON_GATE(g12a_i2c,			HHI_GCLK_MPEG0,	9);
static MESON_GATE(g12a_sana,			HHI_GCLK_MPEG0,	10);
static MESON_GATE(g12a_sd,			HHI_GCLK_MPEG0,	11);
static MESON_GATE(g12a_rng0,			HHI_GCLK_MPEG0,	12);
static MESON_GATE(g12a_uart0,			HHI_GCLK_MPEG0,	13);
static MESON_GATE(g12a_spicc_1,			HHI_GCLK_MPEG0,	14);
static MESON_GATE(g12a_hiu_reg,			HHI_GCLK_MPEG0,	19);
static MESON_GATE(g12a_mipi_dsi_phy,		HHI_GCLK_MPEG0,	20);
static MESON_GATE(g12a_assist_misc,		HHI_GCLK_MPEG0,	23);
static MESON_GATE(g12a_emmc_a,			HHI_GCLK_MPEG0,	4);
static MESON_GATE(g12a_emmc_b,			HHI_GCLK_MPEG0,	25);
static MESON_GATE(g12a_emmc_c,			HHI_GCLK_MPEG0,	26);
static MESON_GATE(g12a_audio_codec,		HHI_GCLK_MPEG0,	28);

static MESON_GATE(g12a_audio,			HHI_GCLK_MPEG1,	0);
static MESON_GATE(g12a_eth_core,		HHI_GCLK_MPEG1,	3);
static MESON_GATE(g12a_demux,			HHI_GCLK_MPEG1,	4);
static MESON_GATE(g12a_audio_ififo,		HHI_GCLK_MPEG1,	11);
static MESON_GATE(g12a_adc,			HHI_GCLK_MPEG1,	13);
static MESON_GATE(g12a_uart1,			HHI_GCLK_MPEG1,	16);
static MESON_GATE(g12a_g2d,			HHI_GCLK_MPEG1,	20);
static MESON_GATE(g12a_reset,			HHI_GCLK_MPEG1,	23);
static MESON_GATE(g12a_pcie_comb,		HHI_GCLK_MPEG1,	24);
static MESON_GATE(g12a_parser,			HHI_GCLK_MPEG1,	25);
static MESON_GATE(g12a_usb_general,		HHI_GCLK_MPEG1,	26);
static MESON_GATE(g12a_pcie_phy,		HHI_GCLK_MPEG1,	27);
static MESON_GATE(g12a_ahb_arb0,		HHI_GCLK_MPEG1,	29);

static MESON_GATE(g12a_ahb_data_bus,		HHI_GCLK_MPEG2,	1);
static MESON_GATE(g12a_ahb_ctrl_bus,		HHI_GCLK_MPEG2,	2);
static MESON_GATE(g12a_htx_hdcp22,		HHI_GCLK_MPEG2,	3);
static MESON_GATE(g12a_htx_pclk,		HHI_GCLK_MPEG2,	4);
static MESON_GATE(g12a_bt656,			HHI_GCLK_MPEG2,	6);
static MESON_GATE(g12a_usb1_to_ddr,		HHI_GCLK_MPEG2,	8);
static MESON_GATE(g12a_mmc_pclk,		HHI_GCLK_MPEG2,	11);
static MESON_GATE(g12a_uart2,			HHI_GCLK_MPEG2,	15);
static MESON_GATE(g12a_vpu_intr,		HHI_GCLK_MPEG2,	25);
static MESON_GATE(g12a_gic,			HHI_GCLK_MPEG2,	30);

static MESON_GATE(g12a_vclk2_venci0,		HHI_GCLK_OTHER,	1);
static MESON_GATE(g12a_vclk2_venci1,		HHI_GCLK_OTHER,	2);
static MESON_GATE(g12a_vclk2_vencp0,		HHI_GCLK_OTHER,	3);
static MESON_GATE(g12a_vclk2_vencp1,		HHI_GCLK_OTHER,	4);
static MESON_GATE(g12a_vclk2_venct0,		HHI_GCLK_OTHER,	5);
static MESON_GATE(g12a_vclk2_venct1,		HHI_GCLK_OTHER,	6);
static MESON_GATE(g12a_vclk2_other,		HHI_GCLK_OTHER,	7);
static MESON_GATE(g12a_vclk2_enci,		HHI_GCLK_OTHER,	8);
static MESON_GATE(g12a_vclk2_encp,		HHI_GCLK_OTHER,	9);
static MESON_GATE(g12a_dac_clk,			HHI_GCLK_OTHER,	10);
static MESON_GATE(g12a_aoclk_gate,		HHI_GCLK_OTHER,	14);
static MESON_GATE(g12a_iec958_gate,		HHI_GCLK_OTHER,	16);
static MESON_GATE(g12a_enc480p,			HHI_GCLK_OTHER,	20);
static MESON_GATE(g12a_rng1,			HHI_GCLK_OTHER,	21);
static MESON_GATE(g12a_vclk2_enct,		HHI_GCLK_OTHER,	22);
static MESON_GATE(g12a_vclk2_encl,		HHI_GCLK_OTHER,	23);
static MESON_GATE(g12a_vclk2_venclmmc,		HHI_GCLK_OTHER,	24);
static MESON_GATE(g12a_vclk2_vencl,		HHI_GCLK_OTHER,	25);
static MESON_GATE(g12a_vclk2_other1,		HHI_GCLK_OTHER,	26);

/* Array of all clocks provided by this provider */
static struct clk_hw_onecell_data g12a_hw_onecell_data = {
	.hws = {
		[CLKID_SYS_PLL]			= &g12a_sys_pll.hw,
		[CLKID_FIXED_PLL]		= &g12a_fixed_pll.hw,
		[CLKID_FCLK_DIV2]		= &g12a_fclk_div2.hw,
		[CLKID_FCLK_DIV3]		= &g12a_fclk_div3.hw,
		[CLKID_FCLK_DIV4]		= &g12a_fclk_div4.hw,
		[CLKID_FCLK_DIV5]		= &g12a_fclk_div5.hw,
		[CLKID_FCLK_DIV7]		= &g12a_fclk_div7.hw,
		[CLKID_FCLK_DIV2P5]		= &g12a_fclk_div2p5.hw,
		[CLKID_GP0_PLL]			= &g12a_gp0_pll.hw,
		[CLKID_MPEG_SEL]		= &g12a_mpeg_clk_sel.hw,
		[CLKID_MPEG_DIV]		= &g12a_mpeg_clk_div.hw,
		[CLKID_CLK81]			= &g12a_clk81.hw,
		[CLKID_MPLL0]			= &g12a_mpll0.hw,
		[CLKID_MPLL1]			= &g12a_mpll1.hw,
		[CLKID_MPLL2]			= &g12a_mpll2.hw,
		[CLKID_MPLL3]			= &g12a_mpll3.hw,
		[CLKID_DDR]			= &g12a_ddr.hw,
		[CLKID_DOS]			= &g12a_dos.hw,
		[CLKID_AUDIO_LOCKER]		= &g12a_audio_locker.hw,
		[CLKID_MIPI_DSI_HOST]		= &g12a_mipi_dsi_host.hw,
		[CLKID_ETH_PHY]			= &g12a_eth_phy.hw,
		[CLKID_ISA]			= &g12a_isa.hw,
		[CLKID_PL301]			= &g12a_pl301.hw,
		[CLKID_PERIPHS]			= &g12a_periphs.hw,
		[CLKID_SPICC0]			= &g12a_spicc_0.hw,
		[CLKID_I2C]			= &g12a_i2c.hw,
		[CLKID_SANA]			= &g12a_sana.hw,
		[CLKID_SD]			= &g12a_sd.hw,
		[CLKID_RNG0]			= &g12a_rng0.hw,
		[CLKID_UART0]			= &g12a_uart0.hw,
		[CLKID_SPICC1]			= &g12a_spicc_1.hw,
		[CLKID_HIU_IFACE]		= &g12a_hiu_reg.hw,
		[CLKID_MIPI_DSI_PHY]		= &g12a_mipi_dsi_phy.hw,
		[CLKID_ASSIST_MISC]		= &g12a_assist_misc.hw,
		[CLKID_SD_EMMC_A]		= &g12a_emmc_a.hw,
		[CLKID_SD_EMMC_B]		= &g12a_emmc_b.hw,
		[CLKID_SD_EMMC_C]		= &g12a_emmc_c.hw,
		[CLKID_AUDIO_CODEC]		= &g12a_audio_codec.hw,
		[CLKID_AUDIO]			= &g12a_audio.hw,
		[CLKID_ETH]			= &g12a_eth_core.hw,
		[CLKID_DEMUX]			= &g12a_demux.hw,
		[CLKID_AUDIO_IFIFO]		= &g12a_audio_ififo.hw,
		[CLKID_ADC]			= &g12a_adc.hw,
		[CLKID_UART1]			= &g12a_uart1.hw,
		[CLKID_G2D]			= &g12a_g2d.hw,
		[CLKID_RESET]			= &g12a_reset.hw,
		[CLKID_PCIE_COMB]		= &g12a_pcie_comb.hw,
		[CLKID_PARSER]			= &g12a_parser.hw,
		[CLKID_USB]			= &g12a_usb_general.hw,
		[CLKID_PCIE_PHY]		= &g12a_pcie_phy.hw,
		[CLKID_AHB_ARB0]		= &g12a_ahb_arb0.hw,
		[CLKID_AHB_DATA_BUS]		= &g12a_ahb_data_bus.hw,
		[CLKID_AHB_CTRL_BUS]		= &g12a_ahb_ctrl_bus.hw,
		[CLKID_HTX_HDCP22]		= &g12a_htx_hdcp22.hw,
		[CLKID_HTX_PCLK]		= &g12a_htx_pclk.hw,
		[CLKID_BT656]			= &g12a_bt656.hw,
		[CLKID_USB1_DDR_BRIDGE]		= &g12a_usb1_to_ddr.hw,
		[CLKID_MMC_PCLK]		= &g12a_mmc_pclk.hw,
		[CLKID_UART2]			= &g12a_uart2.hw,
		[CLKID_VPU_INTR]		= &g12a_vpu_intr.hw,
		[CLKID_GIC]			= &g12a_gic.hw,
		[CLKID_SD_EMMC_B_CLK0_SEL]	= &g12a_sd_emmc_b_clk0_sel.hw,
		[CLKID_SD_EMMC_B_CLK0_DIV]	= &g12a_sd_emmc_b_clk0_div.hw,
		[CLKID_SD_EMMC_B_CLK0]		= &g12a_sd_emmc_b_clk0.hw,
		[CLKID_SD_EMMC_C_CLK0_SEL]	= &g12a_sd_emmc_c_clk0_sel.hw,
		[CLKID_SD_EMMC_C_CLK0_DIV]	= &g12a_sd_emmc_c_clk0_div.hw,
		[CLKID_SD_EMMC_C_CLK0]		= &g12a_sd_emmc_c_clk0.hw,
		[CLKID_MPLL0_DIV]		= &g12a_mpll0_div.hw,
		[CLKID_MPLL1_DIV]		= &g12a_mpll1_div.hw,
		[CLKID_MPLL2_DIV]		= &g12a_mpll2_div.hw,
		[CLKID_MPLL3_DIV]		= &g12a_mpll3_div.hw,
		[CLKID_FCLK_DIV2_DIV]		= &g12a_fclk_div2_div.hw,
		[CLKID_FCLK_DIV3_DIV]		= &g12a_fclk_div3_div.hw,
		[CLKID_FCLK_DIV4_DIV]		= &g12a_fclk_div4_div.hw,
		[CLKID_FCLK_DIV5_DIV]		= &g12a_fclk_div5_div.hw,
		[CLKID_FCLK_DIV7_DIV]		= &g12a_fclk_div7_div.hw,
		[CLKID_FCLK_DIV2P5_DIV]		= &g12a_fclk_div2p5_div.hw,
		[CLKID_HIFI_PLL]		= &g12a_hifi_pll.hw,
		[CLKID_VCLK2_VENCI0]		= &g12a_vclk2_venci0.hw,
		[CLKID_VCLK2_VENCI1]		= &g12a_vclk2_venci1.hw,
		[CLKID_VCLK2_VENCP0]		= &g12a_vclk2_vencp0.hw,
		[CLKID_VCLK2_VENCP1]		= &g12a_vclk2_vencp1.hw,
		[CLKID_VCLK2_VENCT0]		= &g12a_vclk2_venct0.hw,
		[CLKID_VCLK2_VENCT1]		= &g12a_vclk2_venct1.hw,
		[CLKID_VCLK2_OTHER]		= &g12a_vclk2_other.hw,
		[CLKID_VCLK2_ENCI]		= &g12a_vclk2_enci.hw,
		[CLKID_VCLK2_ENCP]		= &g12a_vclk2_encp.hw,
		[CLKID_DAC_CLK]			= &g12a_dac_clk.hw,
		[CLKID_AOCLK]			= &g12a_aoclk_gate.hw,
		[CLKID_IEC958]			= &g12a_iec958_gate.hw,
		[CLKID_ENC480P]			= &g12a_enc480p.hw,
		[CLKID_RNG1]			= &g12a_rng1.hw,
		[CLKID_VCLK2_ENCT]		= &g12a_vclk2_enct.hw,
		[CLKID_VCLK2_ENCL]		= &g12a_vclk2_encl.hw,
		[CLKID_VCLK2_VENCLMMC]		= &g12a_vclk2_venclmmc.hw,
		[CLKID_VCLK2_VENCL]		= &g12a_vclk2_vencl.hw,
		[CLKID_VCLK2_OTHER1]		= &g12a_vclk2_other1.hw,
		[CLKID_FIXED_PLL_DCO]		= &g12a_fixed_pll_dco.hw,
		[CLKID_SYS_PLL_DCO]		= &g12a_sys_pll_dco.hw,
		[CLKID_GP0_PLL_DCO]		= &g12a_gp0_pll_dco.hw,
		[CLKID_HIFI_PLL_DCO]		= &g12a_hifi_pll_dco.hw,
		[NR_CLKS]			= NULL,
	},
	.num = NR_CLKS,
};

/* Convenience table to populate regmap in .probe */
static struct clk_regmap *const g12a_clk_regmaps[] = {
	&g12a_clk81,
	&g12a_dos,
	&g12a_ddr,
	&g12a_audio_locker,
	&g12a_mipi_dsi_host,
	&g12a_eth_phy,
	&g12a_isa,
	&g12a_pl301,
	&g12a_periphs,
	&g12a_spicc_0,
	&g12a_i2c,
	&g12a_sana,
	&g12a_sd,
	&g12a_rng0,
	&g12a_uart0,
	&g12a_spicc_1,
	&g12a_hiu_reg,
	&g12a_mipi_dsi_phy,
	&g12a_assist_misc,
	&g12a_emmc_a,
	&g12a_emmc_b,
	&g12a_emmc_c,
	&g12a_audio_codec,
	&g12a_audio,
	&g12a_eth_core,
	&g12a_demux,
	&g12a_audio_ififo,
	&g12a_adc,
	&g12a_uart1,
	&g12a_g2d,
	&g12a_reset,
	&g12a_pcie_comb,
	&g12a_parser,
	&g12a_usb_general,
	&g12a_pcie_phy,
	&g12a_ahb_arb0,
	&g12a_ahb_data_bus,
	&g12a_ahb_ctrl_bus,
	&g12a_htx_hdcp22,
	&g12a_htx_pclk,
	&g12a_bt656,
	&g12a_usb1_to_ddr,
	&g12a_mmc_pclk,
	&g12a_vpu_intr,
	&g12a_gic,
	&g12a_sd_emmc_b_clk0,
	&g12a_sd_emmc_c_clk0,
	&g12a_mpeg_clk_div,
	&g12a_sd_emmc_b_clk0_div,
	&g12a_sd_emmc_c_clk0_div,
	&g12a_mpeg_clk_sel,
	&g12a_sd_emmc_b_clk0_sel,
	&g12a_sd_emmc_c_clk0_sel,
	&g12a_mpll0,
	&g12a_mpll1,
	&g12a_mpll2,
	&g12a_mpll3,
	&g12a_mpll0_div,
	&g12a_mpll1_div,
	&g12a_mpll2_div,
	&g12a_mpll3_div,
	&g12a_fixed_pll,
	&g12a_sys_pll,
	&g12a_gp0_pll,
	&g12a_hifi_pll,
	&g12a_vclk2_venci0,
	&g12a_vclk2_venci1,
	&g12a_vclk2_vencp0,
	&g12a_vclk2_vencp1,
	&g12a_vclk2_venct0,
	&g12a_vclk2_venct1,
	&g12a_vclk2_other,
	&g12a_vclk2_enci,
	&g12a_vclk2_encp,
	&g12a_dac_clk,
	&g12a_aoclk_gate,
	&g12a_iec958_gate,
	&g12a_enc480p,
	&g12a_rng1,
	&g12a_vclk2_enct,
	&g12a_vclk2_encl,
	&g12a_vclk2_venclmmc,
	&g12a_vclk2_vencl,
	&g12a_vclk2_other1,
	&g12a_fixed_pll_dco,
	&g12a_sys_pll_dco,
	&g12a_gp0_pll_dco,
	&g12a_hifi_pll_dco,
	&g12a_fclk_div2,
	&g12a_fclk_div3,
	&g12a_fclk_div4,
	&g12a_fclk_div5,
	&g12a_fclk_div7,
	&g12a_fclk_div2p5,
};

static const struct clk_ops g12a_clk_no_ops = {};

static struct clk_hw *g12a_clk_hw_register_bypass
				(struct device *dev,
				const char *name,
				const char *parent_name)
{
	struct clk_hw *hw;
	struct clk_init_data init;
	char *clk_name;
	int ret;

	hw = devm_kzalloc(dev, sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return ERR_PTR(-ENOMEM);

	clk_name = kasprintf(GFP_KERNEL, "g12a_%s", name);
	if (!clk_name)
		return ERR_PTR(-ENOMEM);

	init.name = clk_name;
	init.ops = &g12a_clk_no_ops;
	init.flags = 0;
	init.parent_names = parent_name ? &parent_name : NULL;
	init.num_parents = parent_name ? 1 : 0;
	hw->init = &init;

	ret = devm_clk_hw_register(dev, hw);
	kfree(clk_name);

	return ret ? ERR_PTR(ret) : hw;
}

static const struct of_device_id clkc_match_table[] = {
	{ .compatible = "amlogic,g12a-clkc" },
	{}
};

static const struct regmap_config clkc_regmap_config = {
	.reg_bits       = 32,
	.val_bits       = 32,
	.reg_stride     = 4,
};

static int g12a_clkc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct regmap *map;
	struct clk *clk;
	struct clk_hw *hw;
	int ret, i;

	/* Get the hhi system controller node */
	map = syscon_node_to_regmap(of_get_parent(dev->of_node));
	if (IS_ERR(map)) {
		dev_err(dev,
			"failed to get HHI regmap\n");
		return PTR_ERR(map);
	}

	clk = devm_clk_get(dev, "core");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	hw = g12a_clk_hw_register_bypass(dev, "ee_core", __clk_get_name(clk));
	if (IS_ERR(hw))
		return PTR_ERR(hw);

	g12a_hw_onecell_data.hws[CLKID_EE_CORE] = hw;

	/* Populate regmap for the regmap backed clocks */
	for (i = 0; i < ARRAY_SIZE(g12a_clk_regmaps); i++)
		g12a_clk_regmaps[i]->map = map;

	for (i = 0; i < g12a_hw_onecell_data.num; i++) {
		/* array might be sparse */
		if (!g12a_hw_onecell_data.hws[i])
			continue;

		ret = devm_clk_hw_register(dev, g12a_hw_onecell_data.hws[i]);
		if (ret) {
			dev_err(dev, "Clock registration failed\n");
			return ret;
		}
	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get,
					   &g12a_hw_onecell_data);
}

static struct platform_driver g12a_driver = {
	.probe		= g12a_clkc_probe,
	.driver		= {
		.name	= "g12a-clkc",
		.of_match_table = clkc_match_table,
	},
};

builtin_platform_driver(g12a_driver);
