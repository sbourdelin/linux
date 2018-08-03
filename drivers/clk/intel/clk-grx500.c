// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2018 Intel Corporation.
 *  Zhu YiXin <Yixin.zhu@intel.com>
 */

#include <linux/clk-provider.h>
#include <linux/mfd/syscon.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/regmap.h>
#include <linux/spinlock.h>
#include <dt-bindings/clock/intel,grx500-clk.h>

#include "clk-cgu-pll.h"
#include "clk-cgu.h"

#define PLL_DIV_WIDTH		4

/* Gate1 clock shift */
#define G_VCODEC_SHIFT		2
#define G_DMA0_SHIFT		5
#define G_USB0_SHIFT		6
#define G_SPI1_SHIFT		7
#define G_SPI0_SHIFT		8
#define G_CBM_SHIFT		9
#define G_EBU_SHIFT		10
#define G_SSO_SHIFT		11
#define G_GPTC0_SHIFT		12
#define G_GPTC1_SHIFT		13
#define G_GPTC2_SHIFT		14
#define G_UART_SHIFT		17
#define G_CPYTO_SHIFT		20
#define G_SECPT_SHIFT		21
#define G_TOE_SHIFT		22
#define G_MPE_SHIFT		23
#define G_TDM_SHIFT		25
#define G_PAE_SHIFT		26
#define G_USB1_SHIFT		27
#define G_SWITCH_SHIFT		28

/* Gate2 clock shift */
#define G_PCIE0_SHIFT		1
#define G_PCIE1_SHIFT		17
#define G_PCIE2_SHIFT		25

/* Register definition */
#define GRX500_PLL0A_CFG0	0x0004
#define GRX500_PLL0A_CFG1	0x0008
#define GRX500_PLL0B_CFG0	0x0034
#define GRX500_PLL0B_CFG1	0x0038
#define GRX500_LCPLL_CFG0	0x0094
#define GRX500_LCPLL_CFG1	0x0098
#define GRX500_IF_CLK		0x00c4
#define GRX500_CLK_GSR1		0x0120
#define GRX500_CLK_GSR2		0x0130

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

enum grx500_plls {
	pll0a, pll0b, pll3,
};

PNAME(pll_p)	= { "osc" };
PNAME(cpu_p)	= { "cpu0", "cpu1" };

static struct intel_osc_clk grx500_osc_clks[] __initdata = {
	INTEL_OSC(CLK_OSC, "osc", "intel,osc-frequency", 40000000),
};

static struct intel_pll_clk grx500_pll_clks[] __initdata = {
	[pll0a] = INTEL_PLL(CLK_PLL0A, pll_grx500, "pll0a",
		      pll_p, 0, GRX500_PLL0A_CFG0, NULL, 0, 0, 0),
	[pll0b] = INTEL_PLL(CLK_PLL0B, pll_grx500, "pll0b",
		      pll_p, 0, GRX500_PLL0B_CFG0, NULL, 0, 0, 0),
	[pll3] = INTEL_PLL(CLK_PLL3, pll_grx500, "pll3",
		     pll_p, 0, GRX500_LCPLL_CFG0, NULL, 0, 0, 0),
};

static struct intel_clk_branch grx500_branch_clks[] __initdata = {
	INTEL_DIV(CLK_CBM, "cbm", "pll0a", 0, GRX500_PLL0A_CFG1,
		  0, PLL_DIV_WIDTH, 0, 0, pll_div),
	INTEL_DIV(CLK_NGI, "ngi", "pll0a", 0, GRX500_PLL0A_CFG1,
		  4, PLL_DIV_WIDTH, 0, 0, pll_div),
	INTEL_DIV(CLK_SSX4, "ssx4", "pll0a", 0, GRX500_PLL0A_CFG1,
		  8, PLL_DIV_WIDTH, 0, 0, pll_div),
	INTEL_DIV(CLK_CPU0, "cpu0", "pll0a", 0, GRX500_PLL0A_CFG1,
		  12, PLL_DIV_WIDTH, 0, 0, pll_div),
	INTEL_DIV(CLK_PAE, "pae", "pll0b", 0, GRX500_PLL0B_CFG1,
		  0, PLL_DIV_WIDTH, 0, 0, pll_div),
	INTEL_DIV(CLK_GSWIP, "gswip", "pll0b", 0, GRX500_PLL0B_CFG1,
		  4, PLL_DIV_WIDTH, 0, 0, pll_div),
	INTEL_DIV(CLK_DDR, "ddr", "pll0b", 0, GRX500_PLL0B_CFG1,
		  8, PLL_DIV_WIDTH, 0, 0, pll_div),
	INTEL_DIV(CLK_CPU1, "cpu1", "pll0b", 0, GRX500_PLL0B_CFG1,
		  12, PLL_DIV_WIDTH, 0, 0, pll_div),
	INTEL_MUX(CLK_CPU, "cpu", cpu_p, CLK_SET_RATE_PARENT,
		  GRX500_PLL0A_CFG1, 29, 1, 0, 0),
	INTEL_GATE(GCLK_DMA0, "g_dma0", NULL, 0, GRX500_CLK_GSR1,
		   G_DMA0_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_USB0, "g_usb0", NULL, 0, GRX500_CLK_GSR1,
		   G_USB0_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_GPTC0, "g_gptc0", NULL, 0, GRX500_CLK_GSR1,
		   G_GPTC0_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_GPTC1, "g_gptc1", NULL, 0, GRX500_CLK_GSR1,
		   G_GPTC1_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_GPTC2, "g_gptc2", NULL, 0, GRX500_CLK_GSR1,
		   G_GPTC2_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_UART, "g_uart", NULL, 0, GRX500_CLK_GSR1,
		   G_UART_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_PCIE0, "g_pcie0", NULL, 0, GRX500_CLK_GSR2,
		   G_PCIE0_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_PCIE1, "g_pcie1", NULL, 0, GRX500_CLK_GSR2,
		   G_PCIE1_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_PCIE2, "g_pcie2", NULL, 0, GRX500_CLK_GSR2,
		   G_PCIE2_SHIFT, GATE_CLK_HW, 0),
	INTEL_GATE(GCLK_I2C, "g_i2c", NULL, 0, 0, 0, GATE_CLK_VT, 0),
	INTEL_FIXED(CLK_VOICE, "voice", NULL, 0, GRX500_IF_CLK, 14, 2,
		    CLOCK_FLAG_VAL_INIT, 8192000, 2),
	INTEL_FIXED_FACTOR(CLK_DDRPHY, "ddrphy", "ddr", 0, 0, 0,
			   0, 0, 0, 2, 1),
	INTEL_FIXED_FACTOR(CLK_PCIE, "pcie", "pll3", 0, 0, 0,
			   0, 0, 0, 1, 40),
};

static void __init grx500_clk_init(struct device_node *np)
{
	struct intel_clk_provider *ctx;
	struct regmap *map;

	map = syscon_node_to_regmap(np);
	if (IS_ERR(map))
		return;

	ctx = intel_clk_init(np, map, CLK_NR_CLKS);
	if (IS_ERR(ctx)) {
		regmap_exit(map);
		return;
	}

	intel_clk_register_osc(ctx, grx500_osc_clks,
			       ARRAY_SIZE(grx500_osc_clks));
	intel_clk_register_plls(ctx, grx500_pll_clks,
				ARRAY_SIZE(grx500_pll_clks));
	intel_clk_register_branches(ctx, grx500_branch_clks,
				    ARRAY_SIZE(grx500_branch_clks));
	of_clk_add_provider(np, of_clk_src_onecell_get, &ctx->clk_data);

	pr_debug("%s clk init done!\n", __func__);
}

CLK_OF_DECLARE(intel_grx500_cgu, "intel,grx500-cgu", grx500_clk_init);
