// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2016 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 *
 */

#include <linux/clk-provider.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/spinlock.h>
#include <dt-bindings/clock/intel,grx500-clk.h>
#include "clk-cgu-api.h"

/* Intel GRX500 CGU device tree "compatible" strings */
#define INTEL_GRX500_DT_PLL0A_CLK	"intel,grx500-pll0a-clk"
#define INTEL_GRX500_DT_PLL0B_CLK	"intel,grx500-pll0b-clk"
#define INTEL_GRX500_DT_PCIE_CLK	"intel,grx500-pcie-clk"
#define INTEL_GRX500_DT_CPU_CLK		"intel,grx500-cpu-clk"
#define INTEL_GRX500_DT_GATE0_CLK	"intel,grx500-gate0-clk"
#define INTEL_GRX500_DT_GATE1_CLK	"intel,grx500-gate1-clk"
#define INTEL_GRX500_DT_GATE2_CLK	"intel,grx500-gate2-clk"
#define INTEL_GRX500_DT_VOICE_CLK	"intel,grx500-voice-clk"
#define INTEL_GRX500_DT_GATE_I2C_CLK	"intel,grx500-gate-dummy-clk"

/* clock shift and width */
#define CBM_CLK_SHIFT		0
#define CBM_CLK_WIDTH		4
#define NGI_CLK_SHIFT		4
#define NGI_CLK_WIDTH		4
#define SSX4_CLK_SHIFT		8
#define SSX4_CLK_WIDTH		4
#define CPU0_CLK_SHIFT		12
#define CPU0_CLK_WIDTH		4

#define PAE_CLK_SHIFT		0
#define PAE_CLK_WIDTH		4
#define GSWIP_CLK_SHIFT		4
#define GSWIP_CLK_WIDTH		4
#define DDR_CLK_SHIFT		8
#define DDR_CLK_WIDTH		4
#define CPU1_CLK_SHIFT		12
#define CPU1_CLK_WIDTH		4

#define PCIE_CLK_SHIFT		12
#define PCIE_CLK_WIDTH		2

#define CPU_CLK_SHIFT		29
#define CPU_CLK_WIDTH		1

#define VOICE_CLK_SHIFT		14
#define VOICE_CLK_WIDTH		2

/* Gate clock mask */
#define GATE0_CLK_MASK		0xCF
#define GATE1_CLK_MASK		0x1EF27FE4
#define GATE2_CLK_MASK		0x2020002

static const struct clk_div_table pll_div[] = {
	{1,	2},
	{2,	3},
	{3,	4},
	{4,	5},
	{5,	6},
	{6,	8},
	{7,	10},
	{8,	12},
	{9,	16},
	{10,	20},
	{11,	24},
	{12,	32},
	{13,	40},
	{14,	48},
	{15,	64}
};

static const struct gate_dummy_clk_data grx500_clk_gate_i2c_data __initconst = {
	0
};

static void __init grx500_clk_gate_i2c_setup(struct device_node *node)
{
	intel_gate_dummy_clk_setup(node, &grx500_clk_gate_i2c_data);
}

CLK_OF_DECLARE(grx500_gatei2cclk, INTEL_GRX500_DT_GATE_I2C_CLK,
	       grx500_clk_gate_i2c_setup);

static const struct fixed_rate_clk_data grx500_clk_voice_data __initconst = {
	.shift = VOICE_CLK_SHIFT,
	.width = VOICE_CLK_WIDTH,
	.setval = 0x2,
};

static void __init grx500_clk_voice_setup(struct device_node *node)
{
	intel_fixed_rate_clk_setup(node, &grx500_clk_voice_data);
}

CLK_OF_DECLARE(grx500_voiceclk, INTEL_GRX500_DT_VOICE_CLK,
	       grx500_clk_voice_setup);

static const struct gate_clk_data grx500_clk_gate2_data __initconst = {
	.mask = GATE2_CLK_MASK,
	.reg_size = 32,
};

static void __init grx500_clk_gate2_setup(struct device_node *node)
{
	intel_gate_clk_setup(node, &grx500_clk_gate2_data);
}

CLK_OF_DECLARE(grx500_gate2clk, INTEL_GRX500_DT_GATE2_CLK,
	       grx500_clk_gate2_setup);

static const struct gate_clk_data grx500_clk_gate1_data __initconst = {
	.mask = GATE1_CLK_MASK,
	.def_onoff = 0x14000600,
	.reg_size = 32,
	.flags = CLK_INIT_DEF_CFG_REQ,
};

static void __init grx500_clk_gate1_setup(struct device_node *node)
{
	intel_gate_clk_setup(node, &grx500_clk_gate1_data);
}

CLK_OF_DECLARE(grx500_gate1clk, INTEL_GRX500_DT_GATE1_CLK,
	       grx500_clk_gate1_setup);

static const struct gate_clk_data grx500_clk_gate0_data __initconst = {
	.mask = GATE0_CLK_MASK,
	.def_onoff = GATE0_CLK_MASK,
	.reg_size = 32,
	.flags = CLK_INIT_DEF_CFG_REQ,
};

static void __init grx500_clk_gate0_setup(struct device_node *node)
{
	intel_gate_clk_setup(node, &grx500_clk_gate0_data);
}

CLK_OF_DECLARE(grx500_gate0clk, INTEL_GRX500_DT_GATE0_CLK,
	       grx500_clk_gate0_setup);

static const struct mux_clk_data grx500_clk_cpu_data __initconst = {
	.shift = CPU_CLK_SHIFT,
	.width = CPU_CLK_WIDTH,
	.flags = CLK_SET_RATE_PARENT,
};

static void __init grx500_clk_cpu_setup(struct device_node *node)
{
	intel_mux_clk_setup(node, &grx500_clk_cpu_data);
}

CLK_OF_DECLARE(grx500_cpuclk, INTEL_GRX500_DT_CPU_CLK,
	       grx500_clk_cpu_setup);

static const struct div_clk_data grx500_clk_pcie_data __initconst = {
	.shift = PCIE_CLK_SHIFT,
	.width = PCIE_CLK_WIDTH,
	.div_table = pll_div,
};

static void __init grx500_clk_pcie_setup(struct device_node *node)
{
	intel_div_clk_setup(node, &grx500_clk_pcie_data);
}

CLK_OF_DECLARE(grx500_pcieclk, INTEL_GRX500_DT_PCIE_CLK,
	       grx500_clk_pcie_setup);

static const struct div_clk_data grx500_clk_pll0b[] __initconst = {
	{
		.shift = PAE_CLK_SHIFT,
		.width = PAE_CLK_WIDTH,
		.div_table = pll_div,
	},
	{
		.shift = GSWIP_CLK_SHIFT,
		.width = GSWIP_CLK_WIDTH,
		.div_table = pll_div,
	},
	{
		.shift = DDR_CLK_SHIFT,
		.width = DDR_CLK_WIDTH,
		.div_table = pll_div,
	},
	{
		.shift = CPU1_CLK_SHIFT,
		.width = CPU1_CLK_WIDTH,
		.div_table = pll_div,
	},
};

static void __init grx500_clk_pll0b_setup(struct device_node *node)
{
	intel_cluster_div_clk_setup(node, grx500_clk_pll0b,
				    ARRAY_SIZE(grx500_clk_pll0b));
}

CLK_OF_DECLARE(grx500_pll0bclk, INTEL_GRX500_DT_PLL0B_CLK,
	       grx500_clk_pll0b_setup);

static const struct div_clk_data grx500_clk_pll0a[] __initconst = {
	{
		.shift = CBM_CLK_SHIFT,
		.width = CBM_CLK_WIDTH,
		.div_table = pll_div,
	},
	{
		.shift = NGI_CLK_SHIFT,
		.width = NGI_CLK_WIDTH,
		.div_table = pll_div,
	},
	{
		.shift = SSX4_CLK_SHIFT,
		.width = SSX4_CLK_WIDTH,
		.div_table = pll_div,
	},
	{
		.shift = CPU0_CLK_SHIFT,
		.width = CPU0_CLK_WIDTH,
		.div_table = pll_div,
	},
};

static void __init grx500_clk_pll0a_setup(struct device_node *node)
{
	intel_cluster_div_clk_setup(node, grx500_clk_pll0a,
				    ARRAY_SIZE(grx500_clk_pll0a));
}

CLK_OF_DECLARE(grx500_pll0aclk, INTEL_GRX500_DT_PLL0A_CLK,
	       grx500_clk_pll0a_setup);
