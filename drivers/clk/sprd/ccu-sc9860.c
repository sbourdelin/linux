/*
 * Spreatrum SC9860 clock driver
 *
 * Copyright (C) 2017 Spreadtrum, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#include <linux/clk-provider.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/slab.h>

#include "ccu_composite.h"
#include "ccu_div.h"
#include "ccu_gate.h"
#include "ccu_mux.h"
#include "ccu_pll.h"

#include "ccu-sc9860.h"

static CLK_FIXED_FACTOR(fac_4m,		"fac-4m",	"ext-26m",
			6, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_2m,		"fac-2m",	"ext-26m",
			13, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_1m,		"fac-1m",	"ext-26m",
			26, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_250k,	"fac-250k",	"ext-26m",
			104, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_rpll0_26m,	"rpll0-26m",	"ext-26m",
			1, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_rpll1_26m,	"rpll1-26m",	"ext-26m",
			1, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_rco_25m,	"rco-25m",	"ext-rc0-100m",
			4, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_rco_4m,	"rco-4m",	"ext-rc0-100m",
			25, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_rco_2m,	"rco-2m",	"ext-rc0-100m",
			50, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_3k2,	"fac-3k2",	"ext-32k",
			10, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(fac_1k,		"fac-1k",	"ext-32k",
			32, 1, CLK_IS_BASIC);

#define SC9860_GATE_FLAGS (CLK_IGNORE_UNUSED | CLK_IS_BASIC)
static SPRD_CCU_GATE(rpll0_gate,	"rpll0-gate",	"ext-26m", 0x402b016c,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(rpll1_gate,	"rpll1-gate",	"ext-26m", 0x402b016c,
		     0x1000, BIT(18), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mpll0_gate,	"mpll0-gate",	"ext-26m", 0x402b00b0,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mpll1_gate,	"mpll1-gate",	"ext-26m", 0x402b00b0,
		     0x1000, BIT(18), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dpll0_gate,	"dpll0-gate",	"ext-26m", 0x402b00b4,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dpll1_gate,	"dpll1-gate",	"ext-26m", 0x402b00b4,
		     0x1000, BIT(18), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gpll_gate,		"gpll-gate",	"ext-26m", 0x402b032c,
		     0x1000, BIT(0), SC9860_GATE_FLAGS,
		     CLK_GATE_SET_TO_DISABLE);
static SPRD_CCU_GATE(cppll_gate,	"cppll-gate",	"ext-26m", 0x402b02b4,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ltepll0_gate,	"ltepll0-gate",	"ext-26m", 0x402b00b8,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ltepll1_gate,	"ltepll1-gate",	"ext-26m", 0x402b010c,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(twpll_gate,	"twpll-gate",	"ext-26m", 0x402b00bc,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(sdio0_2x_en,	"sdio0-2x-en",	0x402e013c,
			       0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(sdio0_1x_en,	"sdio0-1x-en",	0x402e013c,
			       0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(sdio1_2x_en,	"sdio1-2x-en",	0x402e013c,
			       0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(sdio1_1x_en,	"sdio1-1x-en",	0x402e013c,
			       0x1000, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(sdio2_2x_en,	"sdio2-2x-en",	0x402e013c,
			       0x1000, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(sdio2_1x_en,	"sdio2-1x-en",	0x402e013c,
			       0x1000, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(emmc_1x_en,	"emmc-1x-en",	0x402e013c,
			       0x1000, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(emmc_2x_en,	"emmc-2x-en",	0x402e013c,
			       0x1000, BIT(9), SC9860_GATE_FLAGS, 0);

/* GPLL/LPLL/DPLL/RPLL/CPLL */
static const u64 const itable1[4] = {3, 780000000, 988000000, 1196000000};

/* TWPLL/MPLL0/MPLL1 */
static const u64 itable2[4] = {3, 1638000000, 2080000000, 2600000000UL};

static const struct ccu_bit_field const f_rpll[PLL_FACT_MAX] = {
	{ .shift = 0,	.width = 1 },	/* lock_done	*/
	{ .shift = 3,	.width = 1 },	/* div_s	*/
	{ .shift = 80,	.width = 1 },	/* mod_en	*/
	{ .shift = 81,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 14,	.width = 2 },	/* ibias	*/
	{ .shift = 16,	.width = 7 },	/* n		*/
	{ .shift = 4,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 0,	.width = 0 },	/* postdiv	*/
};
static const u32 const regs_rpll0[4] = { 3, 0x44, 0x48, 0x4c };
static SPRD_CCU_PLL_WITH_ITABLE(rpll0_clk, "rpll0", "rpll0-gate", 0x40400044,
				regs_rpll0, itable1, 200, f_rpll);

static const u32 const regs_rpll1[4] = { 3, 0x50, 0x54, 0x58 };
static SPRD_CCU_PLL_WITH_ITABLE(rpll1_clk, "rpll1", "rpll1-gate", 0x40400050,
				regs_rpll1, itable1, 200, f_rpll);

static const struct ccu_bit_field const f_mpll0[PLL_FACT_MAX] = {
	{ .shift = 20,	.width = 1 },	/* lock_done	*/
	{ .shift = 19,	.width = 1 },	/* div_s	*/
	{ .shift = 18,	.width = 1 },	/* mod_en	*/
	{ .shift = 17,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 11,	.width = 2 },	/* ibias	*/
	{ .shift = 0,	.width = 7 },	/* n		*/
	{ .shift = 57,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 56,	.width = 1 },	/* postdiv	*/
};
static const u32 const regs_mpll0[3] = { 2, 0x24, 0x28 };
static SPRD_CCU_PLL_WITH_ITABLE_FVCO(mpll0_clk, "mpll0", "mpll0-gate",
				     0x40400024, regs_mpll0, itable2,
				     200, f_mpll0, 1300000000, 1);

static const struct ccu_bit_field const f_mpll1[PLL_FACT_MAX] = {
	{ .shift = 20,	.width = 1 },	/* lock_done	*/
	{ .shift = 19,	.width = 1 },	/* div_s	*/
	{ .shift = 18,	.width = 1 },	/* mod_en	*/
	{ .shift = 17,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 11,	.width = 2 },	/* ibias	*/
	{ .shift = 0,	.width = 7 },	/* n		*/
	{ .shift = 57,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 56,	.width = 1 },	/* prediv	*/
	{ .shift = 0,	.width = 0 },	/* postdiv	*/
};
static const u32 const regs_mpll1[3] = { 2, 0x2c, 0x30 };
static SPRD_CCU_PLL_WITH_ITABLE(mpll1_clk, "mpll1", "mpll1-gate", 0x4040002c,
				regs_mpll1, itable2, 200, f_mpll1);

static const struct ccu_bit_field const f_dpll[PLL_FACT_MAX] = {
	{ .shift = 16,	.width = 1 },	/* lock_done	*/
	{ .shift = 15,	.width = 1 },	/* div_s	*/
	{ .shift = 14,	.width = 1 },	/* mod_en	*/
	{ .shift = 13,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 8,	.width = 2 },	/* ibias	*/
	{ .shift = 0,	.width = 7 },	/* n		*/
	{ .shift = 57,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 0,	.width = 0 },	/* postdiv	*/
};
static const u32 const regs_dpll0[3] = { 2, 0x34, 0x38 };
static SPRD_CCU_PLL_WITH_ITABLE(dpll0_clk, "dpll0", "dpll0-gate", 0x40400034,
				regs_dpll0, itable1, 200, f_dpll);

static const u32 const regs_dpll1[3] = { 2, 0x3c, 0x40 };
static SPRD_CCU_PLL_WITH_ITABLE(dpll1_clk, "dpll1", "dpll1-gate", 0x4040003c,
				regs_dpll1, itable1, 200, f_dpll);

static const struct ccu_bit_field const f_gpll[PLL_FACT_MAX] = {
	{ .shift = 18,	.width = 1 },	/* lock_done	*/
	{ .shift = 15,	.width = 1 },	/* div_s	*/
	{ .shift = 14,	.width = 1 },	/* mod_en	*/
	{ .shift = 13,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 8,	.width = 2 },	/* ibias	*/
	{ .shift = 0,	.width = 7 },	/* n		*/
	{ .shift = 57,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 17,	.width = 1 },	/* postdiv	*/
};
static const u32 const regs_gpll[3] = { 2, 0x9c, 0xa0 };
static SPRD_CCU_PLL_WITH_ITABLE_FVCO(gpll_clk, "gpll", "gpll-gate",
				     0x4040009c, regs_gpll, itable1,
				     200, f_gpll, 600000000, 1);

static const struct ccu_bit_field const f_cppll[PLL_FACT_MAX] = {
	{ .shift = 17,	.width = 1 },	/* lock_done	*/
	{ .shift = 15,	.width = 1 },	/* div_s	*/
	{ .shift = 14,	.width = 1 },	/* mod_en	*/
	{ .shift = 13,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 8,	.width = 2 },	/* ibias	*/
	{ .shift = 0,	.width = 7 },	/* n		*/
	{ .shift = 57,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 0,	.width = 0 },	/* postdiv	*/
};
static const u32 const regs_cppll[3] = { 2, 0xc4, 0xc8 };
static SPRD_CCU_PLL_WITH_ITABLE(cppll_clk, "cppll", "cppll-gate", 0x404000c4,
				regs_cppll, itable1, 200, f_cppll);

static const struct ccu_bit_field const f_ltepll[PLL_FACT_MAX] = {
	{ .shift = 31,	.width = 1 },	/* lock_done	*/
	{ .shift = 27,	.width = 1 },	/* div_s	*/
	{ .shift = 26,	.width = 1 },	/* mod_en	*/
	{ .shift = 25,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 20,	.width = 2 },	/* ibias	*/
	{ .shift = 0,	.width = 7 },	/* n		*/
	{ .shift = 57,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 0,	.width = 0 },	/* postdiv	*/
};
static const u32 const regs_ltepll0[3] = { 2, 0x64, 0x68 };
static SPRD_CCU_PLL_WITH_ITABLE(ltepll0_clk, "ltepll0", "ltepll0-gate",
				0x40400064, regs_ltepll0,
				itable1, 200, f_ltepll);
static const u32 const regs_ltepll1[3] = { 2, 0x6c, 0x70 };
static SPRD_CCU_PLL_WITH_ITABLE(ltepll1_clk, "ltepll1", "ltepll1-gate",
				0x4040006c, regs_ltepll1, itable1,
				200, f_ltepll);

static const struct ccu_bit_field const f_twpll[PLL_FACT_MAX] = {
	{ .shift = 21,	.width = 1 },	/* lock_done	*/
	{ .shift = 20,	.width = 1 },	/* div_s	*/
	{ .shift = 19,	.width = 1 },	/* mod_en	*/
	{ .shift = 18,	.width = 1 },	/* sdm_en	*/
	{ .shift = 0,	.width = 0 },	/* refin	*/
	{ .shift = 13,	.width = 2 },	/* ibias	*/
	{ .shift = 0,	.width = 7 },	/* n		*/
	{ .shift = 57,	.width = 7 },	/* nint		*/
	{ .shift = 32,	.width = 23},	/* kint		*/
	{ .shift = 0,	.width = 0 },	/* prediv	*/
	{ .shift = 0,	.width = 0 },	/* postdiv	*/
};
static const u32 const regs_twpll[3] = { 2, 0x5c, 0x60 };
static SPRD_CCU_PLL_WITH_ITABLE(twpll_clk, "twpll", "twpll-gate", 0x4040005c,
				regs_twpll, itable2, 200, f_twpll);

static CLK_FIXED_FACTOR(gpll_42m5, "gpll-42m5", "gpll", 20, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_768m, "twpll-768m", "twpll", 2, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_384m, "twpll-384m", "twpll", 4, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_192m, "twpll-192m", "twpll", 8, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_96m, "twpll-96m", "twpll", 16, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_48m, "twpll-48m", "twpll", 32, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_24m, "twpll-24m", "twpll", 64, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_12m, "twpll-12m", "twpll", 128, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_512m, "twpll-512m", "twpll", 3, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_256m, "twpll-256m", "twpll", 6, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_128m, "twpll-128m", "twpll", 12, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_64m, "twpll-64m", "twpll", 24, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_307m2, "twpll-307m2", "twpll",
			5, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_153m6, "twpll-153m6", "twpll",
			10, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_76m8, "twpll-76m8", "twpll", 20, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_51m2, "twpll-51m2", "twpll", 30, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_38m4, "twpll-38m4", "twpll", 40, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(twpll_19m2, "twpll-19m2", "twpll", 80, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(l0_614m4, "l0-614m4", "ltepll0", 2, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(l0_409m6, "l0-409m6", "ltepll0", 3, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(l0_38m, "l0-38m", "ltepll0", 32, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(l1_38m, "l1-38m", "ltepll1", 32, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(rpll0_192m, "rpll0-192m", "rpll0", 6, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(rpll0_96m, "rpll0-96m", "rpll0", 12, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(rpll0_48m, "rpll0-48m", "rpll0", 24, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(rpll1_468m, "rpll1-468m", "rpll1", 2, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(rpll1_192m, "rpll1-192m", "rpll1", 6, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(rpll1_96m, "rpll1-96m", "rpll1", 12, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(rpll1_64m, "rpll1-64m", "rpll1", 18, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(rpll1_48m, "rpll1-48m", "rpll1", 24, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(dpll0_50m, "dpll0-50m", "dpll0", 16, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(dpll1_50m, "dpll1-50m", "dpll1", 16, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(cppll_50m, "cppll-50m", "cppll", 18, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(m0_39m, "m0-39m", "mpll0", 32, 1, CLK_IS_BASIC);
static CLK_FIXED_FACTOR(m1_63m, "m1-63m", "mpll1", 32, 1, CLK_IS_BASIC);

#define SC9860_COMP_FLAGS (CLK_IGNORE_UNUSED | CLK_IS_BASIC)

static const char * const aon_apb_parents[] = { "rco-25m",	"ext-26m",
						"ext-rco-100m",	"twpll-96m",
						"twpll-128m",
						"twpll-153m6" };
static SPRD_CCU_COMP(aon_apb, "aon-apb", aon_apb_parents, 0x402d0230,
		     0,	0, 3, 8, 2, SC9860_COMP_FLAGS);

static const char * const aux_parents[] = { "ext-32k",		"rpll0-26m",
					    "rpll1-26m",	"ext-26m",
					    "cppll-50m",	"rco-25m",
					    "dpll0-50m",	"dpll1-50m",
					    "gpll-42m5",	"twpll-48m",
					    "m0-39m",		"m1-63m",
					    "l0-38m",		"l1-38m" };

static SPRD_CCU_COMP(aux0_clk,	"aux0",		aux_parents, 0x402d0238,
		     0, 0, 5, 8, 4, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(aux1_clk,	"aux1",		aux_parents, 0x402d023c,
		     0, 0, 5, 8, 4, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(aux2_clk,	"aux2",		aux_parents, 0x402d0240,
		     0, 0, 5, 8, 4, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(probe_clk,	"probe",	aux_parents, 0x402d0244,
		     0, 0, 5, 8, 4, SC9860_COMP_FLAGS);

static const char * const sp_ahb_parents[] = {	"rco-4m",	"ext-26m",
						"ext-rco-100m",	"twpll-96m",
						"twpll-128m",
						"twpll-153m6" };
static SPRD_CCU_COMP(sp_ahb,	"sp-ahb",	sp_ahb_parents, 0x402d02d0,
		     0, 0, 3, 8, 2, SC9860_COMP_FLAGS);

static const char * const cci_parents[] = {	"ext-26m",	"twpll-384m",
						"l0-614m4",	"twpll-768m" };
static SPRD_CCU_COMP(cci_clk,	"cci",		cci_parents, 0x402d0304,
		     0,	0, 2, 8, 2, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(gic_clk,	"gic",		cci_parents, 0x402d0304,
		     0, 0, 2, 8, 2, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(cssys_clk,	"cssys",	cci_parents, 0x402d0310,
		     0, 0, 2, 8, 2, SC9860_COMP_FLAGS);

static const char * const sdio_2x_parents[] = {	"fac-1m",	"ext-26m",
						"twpll-307m2",	"twpll-384m",
						"l0-409m6" };
static SPRD_CCU_COMP(sdio0_2x,	"sdio0-2x",	sdio_2x_parents, 0x402d0328,
		     0, 0, 3, 8, 4, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(sdio1_2x,	"sdio1-2x",	sdio_2x_parents, 0x402d0330,
		     0, 0, 3, 8, 4, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(sdio2_2x,	"sdio2-2x",	sdio_2x_parents, 0x402d0338,
		     0, 0, 3, 8, 4, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(emmc_2x,	"emmc-2x",	sdio_2x_parents, 0x402d0340,
		     0, 0, 3, 8, 4, SC9860_COMP_FLAGS);

static const char * const uart_parents[] = {	"ext-26m",	"twpll-48m",
						"twpll-51m2",	"twpll-96m" };
static SPRD_CCU_COMP(uart0_clk,	"uart0",	uart_parents, 0x20000030,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(uart1_clk,	"uart1",	uart_parents, 0x20000034,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(uart2_clk,	"uart2",	uart_parents, 0x20000038,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(uart3_clk,	"uart3",	uart_parents, 0x2000003c,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(uart4_clk,	"uart4",	uart_parents, 0x20000040,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);

static const char * const i2c_parents[] = { "ext-26m", "twpll-48m",
					    "twpll-51m2", "twpll-153m6" };
static SPRD_CCU_COMP(i2c0_clk,	"i2c0", i2c_parents, 0x20000044,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(i2c1_clk,	"i2c1", i2c_parents, 0x20000048,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(i2c2_clk,	"i2c2", i2c_parents, 0x2000004c,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(i2c3_clk,	"i2c3", i2c_parents, 0x20000050,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(i2c4_clk,	"i2c4", i2c_parents, 0x20000054,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(i2c5_clk,	"i2c5", i2c_parents, 0x20000058,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);

static const char * const spi_parents[] = {	"ext-26m",	"twpll-128m",
						"twpll-153m6",	"twpll-192m" };
static SPRD_CCU_COMP(spi0_clk,	"spi0",	spi_parents, 0x2000005c,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(spi1_clk,	"spi1",	spi_parents, 0x20000060,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(spi2_clk,	"spi2",	spi_parents, 0x20000064,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(spi3_clk,	"spi3",	spi_parents, 0x20000068,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);

static const char * const iis_parents[] = { "ext-26m",
					    "twpll-128m",
					    "twpll-153m6" };
static SPRD_CCU_COMP(iis0_clk,	"iis0",	iis_parents, 0x2000006c,
		     0, 0, 2, 8, 6, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(iis1_clk,	"iis1",	iis_parents, 0x20000070,
		     0, 0, 2, 8, 6, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(iis2_clk,	"iis2",	iis_parents, 0x20000074,
		     0, 0, 2, 8, 6, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(iis3_clk,	"iis3",	iis_parents, 0x20000078,
		     0, 0, 2, 8, 6, SC9860_COMP_FLAGS);

static const u8 mcu_table[] = { 0, 1, 2, 3, 4, 8 };
static const char * const lit_mcu_parents[] = {	"ext-26m",	"twpll-512m",
						"twpll-768m",	"ltepll0",
						"twpll",	"mpll0" };
static SPRD_CCU_COMP(lit_mcu,	"lit-mcu",	lit_mcu_parents, 0x40880020,
		     mcu_table, 0, 4, 4, 3, SC9860_COMP_FLAGS);

static const char * const big_mcu_parents[] = {	"ext-26m",	"twpll-512m",
						"twpll-768m",	"ltepll0",
						"twpll",	"mpll1" };
static SPRD_CCU_COMP(big_mcu,	"big-mcu",	big_mcu_parents, 0x40880024,
		     mcu_table, 0, 4, 4, 3, SC9860_COMP_FLAGS);

static const char * const gpu_parents[] = { "twpll-512m",
					    "twpll-768m",
					    "gpll" };
static SPRD_CCU_COMP(gpu_clk,	"gpu",	gpu_parents, 0x60200020,
		     0, 0, 2, 8, 4, SC9860_COMP_FLAGS);

static const char * const vsp_parents[] = {	"twpll-76m8",	"twpll-128m",
						"twpll-256m",	"twpll-307m2",
						"twpll-384m" };
static SPRD_CCU_COMP(vsp_clk,	"vsp",	vsp_parents, 0x61000024,
		     0, 0, 3, 8, 2, SC9860_COMP_FLAGS);

static const char * const dispc_parents[] = {	"twpll-76m8",	"twpll-128m",
						"twpll-256m",	"twpll-307m2" };
static SPRD_CCU_COMP(vsp_enc, "vsp-enc", dispc_parents, 0x61000028,
		     0, 0, 2, 8, 2, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(dispc0_dpi, "dispc0-dpi", dispc_parents, 0x63000034,
		     0, 0, 2, 8, 2, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(dispc1_dpi, "dispc1-dpi", dispc_parents, 0x63000040,
		     0, 0, 2, 8, 2, SC9860_COMP_FLAGS);

static const char * const sensor_parents[] = {	"ext-26m",	"twpll-48m",
						"twpll-76m8",	"twpll-96m" };
static SPRD_CCU_COMP(sensor0_clk, "sensor0", sensor_parents, 0x62000024,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(sensor1_clk, "sensor1", sensor_parents, 0x62000028,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);
static SPRD_CCU_COMP(sensor2_clk, "sensor2", sensor_parents, 0x6200002c,
		     0, 0, 2, 8, 3, SC9860_COMP_FLAGS);

static SPRD_CCU_DIV(sdio0_1x,	"sdio0-1x",	"sdio0-2x",	0x402d032c,
		    8, 1, CLK_IS_BASIC);
static SPRD_CCU_DIV(sdio1_1x,	"sdio1-1x",	"sdio1-2x",	0x402d0334,
		    8, 1, CLK_IS_BASIC);
static SPRD_CCU_DIV(sdio2_1x,	"sdio2-1x",	"sdio2-2x",	0x402d033c,
		    8, 1, CLK_IS_BASIC);
static SPRD_CCU_DIV(emmc_1x,	"emmc-1x",	"emmc-2x",	0x402d0344,
		    8, 1, CLK_IS_BASIC);

#define SC9860_MUX_FLAG	\
	(CLK_IS_BASIC | CLK_GET_RATE_NOCACHE | CLK_SET_RATE_NO_REPARENT)

static const char * const adi_parents[] = {	"rco-4m",	"ext-26m",
						"rco-25m",	"twpll-38m4",
						"twpll-51m2" };
static SPRD_CCU_MUX(adi_clk,	"adi",	adi_parents, NULL, 0x402d0234,
		    0, 3, SC9860_MUX_FLAG);

static const char * const pwm_parents[] = {	"ext-32k",	"ext-26m",
						"rco-4m",	"rco-25m",
						"twpll-48m" };
static SPRD_CCU_MUX(pwm0_clk,	"pwm0",	pwm_parents, NULL, 0x402d0248,
		    0, 3, SC9860_MUX_FLAG);
static SPRD_CCU_MUX(pwm1_clk,	"pwm1",	pwm_parents, NULL, 0x402d024c,
		    0, 3, SC9860_MUX_FLAG);
static SPRD_CCU_MUX(pwm2_clk,	"pwm2",	pwm_parents, NULL, 0x402d0250,
		    0, 3, SC9860_MUX_FLAG);
static SPRD_CCU_MUX(pwm3_clk,	"pwm3",	pwm_parents, NULL, 0x402d0254,
		    0, 3, SC9860_MUX_FLAG);

static const char * const efuse_parents[] = { "rco-25m", "ext-26m" };
static SPRD_CCU_MUX(efuse_clk, "efuse", efuse_parents, NULL, 0x402d0258,
		    0, 1, SC9860_MUX_FLAG);

static const char * const cm3_uart_parents[] = { "rco-4m",	"ext-26m",
						 "rco-100m",	"twpll-48m",
						 "twpll-51m2",	"twpll-96m",
						 "twpll-128m" };
static SPRD_CCU_MUX(cm3_uart0, "cm3-uart0", cm3_uart_parents, NULL, 0x402d025c,
		    0, 3, SC9860_MUX_FLAG);
static SPRD_CCU_MUX(cm3_uart1, "cm3-uart1", cm3_uart_parents, NULL, 0x402d0260,
		    0, 3, SC9860_MUX_FLAG);

static const char * const thm_parents[] = { "ext-32k", "fac-250k" };
static SPRD_CCU_MUX(thm_clk,	"thm",	thm_parents, NULL, 0x402d0270,
		    0, 1, SC9860_MUX_FLAG);

static const char * const cm3_i2c_parents[] = {	"rco-4m",
						"ext-26m",
						"rco-100m",
						"twpll-48m",
						"twpll-51m2",
						"twpll-153m6" };
static SPRD_CCU_MUX(cm3_i2c0, "cm3-i2c0", cm3_i2c_parents, NULL, 0x402d0274,
		    0, 3, SC9860_MUX_FLAG);
static SPRD_CCU_MUX(cm3_i2c1, "cm3-i2c1", cm3_i2c_parents, NULL, 0x402d0278,
		    0, 3, SC9860_MUX_FLAG);
static SPRD_CCU_MUX(aon_i2c, "aon-i2c",	cm3_i2c_parents, NULL, 0x402d0280,
		    0, 3, SC9860_MUX_FLAG);

static const char * const cm4_spi_parents[] = {	"ext-26m",	"twpll-96m",
						"rco-100m",	"twpll-128m",
						"twpll-153m6",	"twpll-192m" };
static SPRD_CCU_MUX(cm4_spi, "cm4-spi", cm4_spi_parents, NULL, 0x402d027c,
		    0, 3, SC9860_MUX_FLAG);

static SPRD_CCU_MUX(avs_clk, "avs", uart_parents, NULL, 0x402d0284,
		    0, 2, SC9860_MUX_FLAG);

static const char * const ca53_dap_parents[] = { "ext-26m",	"rco-4m",
						 "rco-100m",	"twpll-76m8",
						 "twpll-128m",	"twpll-153m6" };
static SPRD_CCU_MUX(ca53_dap, "ca53-dap", ca53_dap_parents, NULL, 0x402d0288,
		    0, 3, SC9860_MUX_FLAG);

static const char * const ca53_ts_parents[] = {	"ext-32k", "ext-26m",
						"clk-twpll-128m",
						"clk-twpll-153m6" };
static SPRD_CCU_MUX(ca53_ts, "ca53-ts", ca53_ts_parents, NULL, 0x402d0290,
		    0, 2, SC9860_MUX_FLAG);

static const char * const djtag_tck_parents[] = { "rco-4m", "ext-26m" };
static SPRD_CCU_MUX(djtag_tck, "djtag-tck", djtag_tck_parents, NULL,
	0x402d02c8, 0, 1, SC9860_MUX_FLAG);

static const char * const pmu_parents[] = { "ext-32k", "rco-4m", "clk-4m" };
static SPRD_CCU_MUX(pmu_clk, "pmu", pmu_parents, NULL, 0x402d02e0,
		    0, 2, SC9860_MUX_FLAG);

static const char * const pmu_26m_parents[] = { "rco-25m", "ext-26m" };
static SPRD_CCU_MUX(pmu_26m, "pmu-26m", pmu_26m_parents, NULL,
		    0x402d02e4, 0, 1, SC9860_MUX_FLAG);

static const char * const debounce_parents[] = { "ext-32k", "rco-4m",
						 "rco-25m", "ext-26m" };
static SPRD_CCU_MUX(debounce_clk, "debounce", debounce_parents, NULL,
		    0x402d02e8, 0, 2, SC9860_MUX_FLAG);

static const char * const otg2_ref_parents[] = { "twpll-12m", "twpll-24m" };
static SPRD_CCU_MUX(otg2_ref, "otg2-ref", otg2_ref_parents, NULL,
		    0x402d02f4, 0, 1, SC9860_MUX_FLAG);

static const char * const usb3_ref_parents[] = { "twpll-24m", "twpll-19m2",
						 "twpll-48m" };
static SPRD_CCU_MUX(usb3_ref, "usb3-ref", usb3_ref_parents, NULL,
		    0x402d02f8, 0, 2, SC9860_MUX_FLAG);

static const char * const ap_axi_parents[] = { "ext-26m", "twpll-76m8",
					       "twpll-128m", "twpll-256m" };
static SPRD_CCU_MUX(ap_axi, "ap-axi", ap_axi_parents, NULL,
		    0x402d0324, 0, 2, SC9860_MUX_FLAG);

static const char * const ap_apb_parents[] = { "ext-26m", "twpll-64m",
					       "twpll-96m", "twpll-128m" };
static SPRD_CCU_MUX(ap_apb, "ap-apb", ap_apb_parents, NULL,
		    0x20000020, 0, 1, SC9860_MUX_FLAG);

static const char * const ahb_parents[] = { "ext-26m", "twpll-96m",
					    "twpll-128m", "twpll-153m6" };
static SPRD_CCU_MUX(ahb_vsp, "ahb-vsp", ahb_parents, NULL,
		    0x61000020, 0, 2, SC9860_MUX_FLAG);
static SPRD_CCU_MUX(ahb_disp, "ahb-disp", ahb_parents, NULL,
		    0x63000020, 0, 2, SC9860_MUX_FLAG);
static SPRD_CCU_MUX(ahb_cam, "ahb-cam", ahb_parents, NULL,
		    0x62000020, 0, 2, SC9860_MUX_FLAG);

static SPRD_CCU_GATE(mipi_csi0_eb, "mipi-csi0-eb", "ahb-cam", 0x6200004c,
		     0, BIT(16), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mipi_csi1_eb, "mipi-csi1-eb", "ahb-cam", 0x62000050,
		     0, BIT(16), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(usb3_eb,		"usb3-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(usb3_suspend,	"usb3-suspend", "ap-axi", 0x20210000,
		     0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(usb3_ref_eb,	"usb3-ref-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dma_eb,		"dma-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(sdio0_eb,		"sdio0-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(sdio1_eb,		"sdio1-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(sdio2_eb,		"sdio2-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(emmc_eb,		"emmc-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(rom_eb,		"rom-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(12), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(busmon_eb,		"busmon-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(cc63s_eb,		"cc63s-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(22), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(cc63p_eb,		"cc63p-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(23), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ce0_eb,		"ce0-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(24), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ce1_eb,		"ce1-eb",	"ap-axi", 0x20210000,
		     0x1000, BIT(25), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(avs_lit_eb,	"avs-lit-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(avs_big_eb,	"avs-big-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_intc5_eb,	"ap-intc5-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gpio_eb,		"gpio-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(pwm0_eb,		"pwm0-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(pwm1_eb,		"pwm1-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(pwm2_eb,		"pwm2-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(pwm3_eb,		"pwm3-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(kpd_eb,		"kpd-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(aon_sys_eb,	"aon-sys-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_sys_eb,		"ap-sys-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(aon_tmr_eb,	"aon-tmr-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(11), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_tmr0_eb,	"ap-tmr0-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(12), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(efuse_eb,		"efuse-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(eic_eb,		"eic-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(14), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(pub1_reg_eb,	"pub1-reg-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(15), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(adi_eb,		"adi-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(16), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_intc0_eb,	"ap-intc0-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(17), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_intc1_eb,	"ap-intc1-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(18), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_intc2_eb,	"ap-intc2-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(19), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_intc3_eb,	"ap-intc3-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(20), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_intc4_eb,	"ap-intc4-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(21), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(splk_eb,		"splk-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(22), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mspi_eb,		"mspi-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(23), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(pub0_reg_eb,	"pub0-reg-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(24), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(pin_eb,		"pin-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(25), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(aon_ckg_eb,	"aon-ckg-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(26), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gpu_eb,		"gpu-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(27), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(apcpu_ts0_eb,	"apcpu-ts0-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(28), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(apcpu_ts1_eb,	"apcpu-ts1-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(29), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dap_eb,		"dap-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(30), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(i2c_eb,		"i2c-eb",	"aon-apb", 0x402e0000,
		     0x1000, BIT(31), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(pmu_eb,		"pmu-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(thm_eb,		"thm-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(aux0_eb,		"aux0-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(aux1_eb,		"aux1-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(aux2_eb,		"aux2-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(probe_eb,		"probe-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gpu0_avs_eb,	"gpu0-avs-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gpu1_avs_eb,	"gpu1-avs-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(apcpu_wdg_eb,	"apcpu-wdg-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_tmr1_eb,	"ap-tmr1-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_tmr2_eb,	"ap-tmr2-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(disp_emc_eb,	"disp-emc-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(11), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(zip_emc_eb,	"zip-emc-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(12), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp_emc_eb,	"gsp-emc-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(osc_aon_eb,	"osc-aon-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(14), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(lvds_trx_eb,	"lvds-trx-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(15), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(lvds_tcxo_eb,	"lvds-tcxo-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(16), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mdar_eb,		"mdar-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(17), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(rtc4m0_cal_eb, "rtc4m0-cal-eb", "aon-apb", 0x402e0004,
		     0x1000, BIT(18), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(rct100m_cal_eb, "rct100m-cal-eb", "aon-apb", 0x402e0004,
		     0x1000, BIT(19), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(djtag_eb,		"djtag-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(20), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mbox_eb,		"mbox-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(21), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(aon_dma_eb,	"aon-dma-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(22), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dbg_emc_eb,	"dbg-emc-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(23), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(lvds_pll_div_en, "lvds-pll-div-en", "aon-apb", 0x402e0004,
		     0x1000, BIT(24), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(def_eb,		"def-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(25), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(aon_apb_rsv0,	"aon-apb-rsv0",	"aon-apb", 0x402e0004,
		     0x1000, BIT(26), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(orp_jtag_eb,	"orp-jtag-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(27), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vsp_eb,		"vsp-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(28), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(cam_eb,		"cam-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(29), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(disp_eb,		"disp-eb",	"aon-apb", 0x402e0004,
		     0x1000, BIT(30), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dbg_axi_if_eb,	"dbg-axi-if-eb", "aon-apb", 0x402e0004,
		     0x1000, BIT(31), SC9860_GATE_FLAGS, 0);

static SPRD_CCU_GATE_NO_PARENT(agcp_iis0_eb, "agcp-iis0-eb", 0x415e0000,
			       0x100, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_iis1_eb, "agcp-iis1-eb", 0x415e0000,
			       0x100, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_iis2_eb, "agcp-iis2-eb", 0x415e0000,
			       0x100, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_iis3_eb, "agcp-iis3-eb", 0x415e0000,
			       0x100, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_uart_eb, "agcp-uart-eb", 0x415e0000,
			       0x100, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_dmacp_eb, "agcp-dmacp-eb", 0x415e0000,
			       0x100, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_dmaap_eb, "agcp-dmaap-eb", 0x415e0000,
			       0x100, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_arc48k_eb, "agcp-arc48k-eb", 0x415e0000,
			       0x100, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_src44p1k_eb, "agcp-src44p1k-eb", 0x415e0000,
			       0x100, BIT(11), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_mcdt_eb, "agcp-mcdt-eb", 0x415e0000,
			       0x100, BIT(12), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_vbcifd_eb, "agcp-vbcifd-eb", 0x415e0000,
			       0x100, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_vbc_eb, "agcp-vbc-eb", 0x415e0000,
			       0x100, BIT(14), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_spinlock_eb, "agcp-spinlock-eb", 0x415e0000,
			       0x100, BIT(15), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_icu_eb, "agcp-icu-eb", 0x415e0000,
			       0x100, BIT(16), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_ap_ashb_eb, "agcp-ap-ashb-eb", 0x415e0000,
			       0x100, BIT(17), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_cp_ashb_eb,	"agcp-cp-ashb-eb", 0x415e0000,
			       0x100, BIT(18), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_aud_eb, "agcp-aud-eb", 0x415e0000,
			       0x100, BIT(19), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE_NO_PARENT(agcp_audif_eb, "agcp-audif-eb", 0x415e0000,
			       0x100, BIT(20), SC9860_GATE_FLAGS, 0);

static SPRD_CCU_GATE(vsp_dec_eb,	"vsp-dec-eb",	"ahb_vsp", 0x61100000,
		     0x1000, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vsp_ckg_eb,	"vsp-ckg-eb",	"ahb_vsp", 0x61100000,
		     0x1000, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vsp_mmu_eb,	"vsp-mmu-eb",	"ahb_vsp", 0x61100000,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vsp_enc_eb,	"vsp-enc-eb",	"ahb_vsp", 0x61100000,
		     0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vpp_eb,		"vpp-eb",	"ahb_vsp", 0x61100000,
		     0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vsp_26m_eb,	"vsp-26m-eb",	"ahb_vsp", 0x61100000,
		     0x1000, BIT(5), SC9860_GATE_FLAGS, 0);

static SPRD_CCU_GATE(vsp_axi_gate,	"vsp-axi-gate",	"ahb_vsp", 0x61100008,
		     0, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vsp_enc_gate,	"vsp-enc-gate",	"ahb_vsp", 0x61100008,
		     0, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vpp_axi_gate,	"vpp-axi-gate",	"ahb_vsp", 0x61100008,
		     0, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vsp_bm_gate,	"vsp-bm-gate",	"ahb_vsp", 0x61100008,
		     0, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vsp_enc_bm_gate, "vsp-enc-bm-gate", "ahb_vsp", 0x61100008,
		     0, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(vpp_bm_gate, "vpp-bm-gate", "ahb_vsp", 0x61100008,
		     0, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dcam0_eb,		"dcam0-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dcam1_eb,		"dcam1-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp0_eb,		"isp0-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(csi0_eb,		"csi0-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(csi1_eb,		"csi1-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(jpg0_eb,		"jpg0-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(jpg1_eb,		"jpg1-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(cam_ckg_eb,	"cam-ckg-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(cam_mmu_eb,	"cam-mmu-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp1_eb,		"isp1-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(cpp_eb,		"cpp-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mmu_pf_eb,		"mmu-pf-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(11), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp2_eb,		"isp2-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(12), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dcam2isp_if_eb, "dcam2isp-if-eb", "ahb_cam", 0x62100000,
		     0x1000, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp2dcam_if_eb, "isp2dcam-if-eb", "ahb_cam", 0x62100000,
		     0x1000, BIT(14), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp_lclk_eb,	"isp-lclk-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(15), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp_iclk_eb,	"isp-iclk-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(16), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp_mclk_eb,	"isp-mclk-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(17), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp_pclk_eb,	"isp-pclk-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(18), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp_isp2dcam_eb, "isp-isp2dcam-eb", "ahb_cam", 0x62100000,
		     0x1000, BIT(19), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dcam0_if_eb,	"dcam0-if-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(20), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(clk26m_if_eb,	"clk26m-if-eb",	"ahb-cam", 0x62100000,
		     0x1000, BIT(21), SC9860_GATE_FLAGS, 0);

static SPRD_CCU_GATE(cphy0_gate, "cphy0-gate", "ahb-cam", 0x62100008,
		     0, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mipi_csi0_gate, "mipi-csi0-gate", "ahb_cam", 0x62100008,
		     0, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(cphy1_gate,	"cphy1-gate",	"ahb-cam", 0x62100008,
		     0, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(mipi_csi1,		"mipi-csi1",	"ahb-cam", 0x62100008,
		     0, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dcam0_axi_gate,	"dcam0-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dcam1_axi_gate,	"dcam1-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(sensor0_gate,	"sensor0-gate",	"ahb-cam", 0x62100008,
		     0, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(sensor1_gate,	"sensor1-gate",	"ahb-cam", 0x62100008,
		     0, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(jpg0_axi_gate,	"jpg0-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gpg1_axi_gate,	"gpg1-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp0_axi_gate,	"isp0-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp1_axi_gate,	"isp1-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(11), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(isp2_axi_gate,	"isp2-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(12), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(cpp_axi_gate,	"cpp-axi-gate",	"ahb-cam", 0x62100008,
		     0, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d0_if_axi_gate,	"d0-if-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(14), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d2i_if_axi_gate, "d2i-if-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(15), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(i2d_if_axi_gate, "i2d-if-axi-gate", "ahb_cam", 0x62100008,
		     0, BIT(16), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(spare_axi_gate, "spare-axi-gate",	"ahb_cam", 0x62100008,
		     0, BIT(17), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(sensor2_gate, "sensor2-gate",	"ahb-cam", 0x62100008,
		     0, BIT(18), SC9860_GATE_FLAGS, 0);

static SPRD_CCU_GATE(d0if_in_d_en, "d0if-in-d-en", "ahb-cam", 0x62100028,
		     0x1000, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d1if_in_d_en, "d1if-in-d-en", "ahb-cam", 0x62100028,
		     0x1000, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d0if_in_d2i_en, "d0if-in-d2i-en", "ahb_cam", 0x62100028,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d1if_in_d2i_en, "d1if-in-d2i-en",	"ahb_cam", 0x62100028,
		     0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ia_in_d2i_en, "ia-in-d2i-en",	"ahb-cam", 0x62100028,
		     0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ib_in_d2i_en,	"ib-in-d2i-en",	"ahb-cam", 0x62100028,
		     0x1000, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ic_in_d2i_en,	"ic-in-d2i-en",	"ahb-cam", 0x62100028,
		     0x1000, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ia_in_i_en,	"ia-in-i-en",	"ahb-cam", 0x62100028,
		     0x1000, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ib_in_i_en,	"ib-in-i-en",	"ahb-cam", 0x62100028,
		     0x1000, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ic_in_i_en,	"ic-in-i-en",	"ahb-cam", 0x62100028,
		     0x1000, BIT(9), SC9860_GATE_FLAGS, 0);

static SPRD_CCU_GATE(dispc0_eb,		"dispc0-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dispc1_eb,		"dispc1-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dispc_mmu_eb,	"dispc-mmu-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp0_eb,		"gsp0-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp1_eb,		"gsp1-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp0_mmu_eb,	"gsp0-mmu-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp1_mmu_eb,	"gsp1-mmu-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dsi0_eb,		"dsi0-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dsi1_eb,		"dsi1-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(disp_ckg_eb,	"disp-ckg-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(disp_gpu_eb,	"disp-gpu-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gpu_mtx_eb,	"gpu-mtx-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp_mtx_eb,	"gsp-mtx-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(14), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(tmc_mtx_eb,	"tmc-mtx-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(15), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dispc_mtx_eb,	"dispc-mtx-eb",	"ahb-disp", 0x63100000,
		     0x1000, BIT(16), SC9860_GATE_FLAGS, 0);

static SPRD_CCU_GATE(dphy0_gate,	"dphy0-gate",	"ahb-disp", 0x63100008,
		     0, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dphy1_gate,	"dphy1-gate",	"ahb-disp", 0x63100008,
		     0, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp0_a_gate,	"gsp0-a-gate",	"ahb-disp", 0x63100008,
		     0, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp1_a_gate,	"gsp1-a-gate",	"ahb-disp", 0x63100008,
		     0, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp0_f_gate,	"gsp0-f-gate",	"ahb-disp", 0x63100008,
		     0, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp1_f_gate,	"gsp1-f-gate",	"ahb-disp", 0x63100008,
		     0, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d_mtx_f_gate,	"d-mtx-f-gate",	"ahb-disp", 0x63100008,
		     0, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d_mtx_a_gate,	"d-mtx-a-gate",	"ahb-disp", 0x63100008,
		     0, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d_noc_f_gate,	"d-noc-f-gate",	"ahb-disp", 0x63100008,
		     0, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(d_noc_a_gate,	"d-noc-a-gate",	"ahb-disp", 0x63100008,
		     0, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp_mtx_f_gate, "gsp-mtx-f-gate", "ahb-disp", 0x63100008,
		     0, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp_mtx_a_gate, "gsp-mtx-a-gate", "ahb-disp", 0x63100008,
		     0, BIT(11), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp_noc_f_gate, "gsp-noc-f-gate", "ahb-disp", 0x63100008,
		     0, BIT(12), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gsp_noc_a_gate, "gsp-noc-a-gate", "ahb-disp", 0x63100008,
		     0, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(dispm0idle_gate, "dispm0idle-gate", "ahb-disp", 0x63100008,
		     0, BIT(14), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(gspm0idle_gate, "gspm0idle-gate", "ahb-disp", 0x63100008,
		     0, BIT(15), SC9860_GATE_FLAGS, 0);

static SPRD_CCU_GATE(sim0_eb,	"sim0-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(0), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(iis0_eb,	"iis0-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(1), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(iis1_eb,	"iis1-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(2), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(iis2_eb,	"iis2-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(3), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(iis3_eb,	"iis3-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(4), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(spi0_eb,	"spi0-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(5), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(spi1_eb,	"spi1-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(6), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(spi2_eb,	"spi2-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(7), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(i2c0_eb,	"i2c0-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(8), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(i2c1_eb,	"i2c1-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(9), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(i2c2_eb,	"i2c2-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(10), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(i2c3_eb,	"i2c3-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(11), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(i2c4_eb,	"i2c4-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(12), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(i2c5_eb,	"i2c5-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(13), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(uart0_eb,	"uart0-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(14), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(uart1_eb,	"uart1-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(15), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(uart2_eb,	"uart2-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(16), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(uart3_eb,	"uart3-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(17), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(uart4_eb,	"uart4-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(18), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(ap_ckg_eb,	"ap-ckg-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(19), SC9860_GATE_FLAGS, 0);
static SPRD_CCU_GATE(spi3_eb,	"spi3-eb",	"ap_apb", 0x70b00000,
		     0x1000, BIT(20), SC9860_GATE_FLAGS, 0);

static struct ccu_common *sc9860_ccu_clks[] = {
	&rpll0_gate.common,
	&rpll1_gate.common,
	&mpll0_gate.common,
	&mpll1_gate.common,
	&dpll0_gate.common,
	&dpll1_gate.common,
	&gpll_gate.common,
	&cppll_gate.common,
	&ltepll0_gate.common,
	&ltepll1_gate.common,
	&twpll_gate.common,
	&sdio0_2x_en.common,
	&sdio0_1x_en.common,
	&sdio1_2x_en.common,
	&sdio1_1x_en.common,
	&sdio2_2x_en.common,
	&sdio2_1x_en.common,
	&emmc_1x_en.common,
	&emmc_2x_en.common,
	&rpll0_clk.common,
	&rpll1_clk.common,
	&mpll0_clk.common,
	&mpll1_clk.common,
	&dpll0_clk.common,
	&dpll1_clk.common,
	&gpll_clk.common,
	&cppll_clk.common,
	&ltepll0_clk.common,
	&ltepll1_clk.common,
	&twpll_clk.common,
	&aon_apb.common,
	&aux0_clk.common,
	&aux1_clk.common,
	&aux2_clk.common,
	&probe_clk.common,
	&sp_ahb.common,
	&cci_clk.common,
	&gic_clk.common,
	&cssys_clk.common,
	&sdio0_2x.common,
	&sdio1_2x.common,
	&sdio2_2x.common,
	&emmc_2x.common,
	&uart0_clk.common,
	&uart1_clk.common,
	&uart2_clk.common,
	&uart3_clk.common,
	&uart4_clk.common,
	&i2c0_clk.common,
	&i2c1_clk.common,
	&i2c2_clk.common,
	&i2c3_clk.common,
	&i2c4_clk.common,
	&i2c5_clk.common,
	&spi0_clk.common,
	&spi1_clk.common,
	&spi2_clk.common,
	&spi3_clk.common,
	&iis0_clk.common,
	&iis1_clk.common,
	&iis2_clk.common,
	&iis3_clk.common,
	&lit_mcu.common,
	&big_mcu.common,
	&gpu_clk.common,
	&vsp_clk.common,
	&vsp_enc.common,
	&dispc0_dpi.common,
	&dispc1_dpi.common,
	&sensor0_clk.common,
	&sensor1_clk.common,
	&sensor2_clk.common,
	&sdio0_1x.common,
	&sdio1_1x.common,
	&sdio2_1x.common,
	&emmc_1x.common,
	&adi_clk.common,
	&pwm0_clk.common,
	&pwm1_clk.common,
	&pwm2_clk.common,
	&pwm3_clk.common,
	&efuse_clk.common,
	&cm3_uart0.common,
	&cm3_uart1.common,
	&thm_clk.common,
	&cm3_i2c0.common,
	&cm3_i2c1.common,
	&cm4_spi.common,
	&aon_i2c.common,
	&avs_clk.common,
	&ca53_dap.common,
	&ca53_ts.common,
	&djtag_tck.common,
	&pmu_clk.common,
	&pmu_26m.common,
	&debounce_clk.common,
	&otg2_ref.common,
	&usb3_ref.common,
	&ap_axi.common,
	&ap_apb.common,
	&ahb_vsp.common,
	&ahb_disp.common,
	&ahb_cam.common,
	&mipi_csi0_eb.common,
	&mipi_csi1_eb.common,
	&usb3_eb.common,
	&usb3_suspend.common,
	&usb3_ref_eb.common,
	&dma_eb.common,
	&sdio0_eb.common,
	&sdio1_eb.common,
	&sdio2_eb.common,
	&emmc_eb.common,
	&rom_eb.common,
	&busmon_eb.common,
	&cc63s_eb.common,
	&cc63p_eb.common,
	&ce0_eb.common,
	&ce1_eb.common,
	&avs_lit_eb.common,
	&avs_big_eb.common,
	&ap_intc5_eb.common,
	&gpio_eb.common,
	&pwm0_eb.common,
	&pwm1_eb.common,
	&pwm2_eb.common,
	&pwm3_eb.common,
	&kpd_eb.common,
	&aon_sys_eb.common,
	&ap_sys_eb.common,
	&aon_tmr_eb.common,
	&ap_tmr0_eb.common,
	&efuse_eb.common,
	&eic_eb.common,
	&pub1_reg_eb.common,
	&adi_eb.common,
	&ap_intc0_eb.common,
	&ap_intc1_eb.common,
	&ap_intc2_eb.common,
	&ap_intc3_eb.common,
	&ap_intc4_eb.common,
	&splk_eb.common,
	&mspi_eb.common,
	&pub0_reg_eb.common,
	&pin_eb.common,
	&aon_ckg_eb.common,
	&gpu_eb.common,
	&apcpu_ts0_eb.common,
	&apcpu_ts1_eb.common,
	&dap_eb.common,
	&i2c_eb.common,
	&pmu_eb.common,
	&thm_eb.common,
	&aux0_eb.common,
	&aux1_eb.common,
	&aux2_eb.common,
	&probe_eb.common,
	&gpu0_avs_eb.common,
	&gpu1_avs_eb.common,
	&apcpu_wdg_eb.common,
	&ap_tmr1_eb.common,
	&ap_tmr2_eb.common,
	&disp_emc_eb.common,
	&zip_emc_eb.common,
	&gsp_emc_eb.common,
	&osc_aon_eb.common,
	&lvds_trx_eb.common,
	&lvds_tcxo_eb.common,
	&mdar_eb.common,
	&rtc4m0_cal_eb.common,
	&rct100m_cal_eb.common,
	&djtag_eb.common,
	&mbox_eb.common,
	&aon_dma_eb.common,
	&dbg_emc_eb.common,
	&lvds_pll_div_en.common,
	&def_eb.common,
	&aon_apb_rsv0.common,
	&orp_jtag_eb.common,
	&vsp_eb.common,
	&cam_eb.common,
	&disp_eb.common,
	&dbg_axi_if_eb.common,
	&agcp_iis0_eb.common,
	&agcp_iis1_eb.common,
	&agcp_iis2_eb.common,
	&agcp_iis3_eb.common,
	&agcp_uart_eb.common,
	&agcp_dmacp_eb.common,
	&agcp_dmaap_eb.common,
	&agcp_arc48k_eb.common,
	&agcp_src44p1k_eb.common,
	&agcp_mcdt_eb.common,
	&agcp_vbcifd_eb.common,
	&agcp_vbc_eb.common,
	&agcp_spinlock_eb.common,
	&agcp_icu_eb.common,
	&agcp_ap_ashb_eb.common,
	&agcp_cp_ashb_eb.common,
	&agcp_aud_eb.common,
	&agcp_audif_eb.common,
	&vsp_dec_eb.common,
	&vsp_ckg_eb.common,
	&vsp_mmu_eb.common,
	&vsp_enc_eb.common,
	&vpp_eb.common,
	&vsp_26m_eb.common,
	&vsp_axi_gate.common,
	&vsp_enc_gate.common,
	&vpp_axi_gate.common,
	&vsp_bm_gate.common,
	&vsp_enc_bm_gate.common,
	&vpp_bm_gate.common,
	&dcam0_eb.common,
	&dcam1_eb.common,
	&isp0_eb.common,
	&csi0_eb.common,
	&csi1_eb.common,
	&jpg0_eb.common,
	&jpg1_eb.common,
	&cam_ckg_eb.common,
	&cam_mmu_eb.common,
	&isp1_eb.common,
	&cpp_eb.common,
	&mmu_pf_eb.common,
	&isp2_eb.common,
	&dcam2isp_if_eb.common,
	&isp2dcam_if_eb.common,
	&isp_lclk_eb.common,
	&isp_iclk_eb.common,
	&isp_mclk_eb.common,
	&isp_pclk_eb.common,
	&isp_isp2dcam_eb.common,
	&dcam0_if_eb.common,
	&clk26m_if_eb.common,
	&cphy0_gate.common,
	&mipi_csi0_gate.common,
	&cphy1_gate.common,
	&mipi_csi1.common,
	&dcam0_axi_gate.common,
	&dcam1_axi_gate.common,
	&sensor0_gate.common,
	&sensor1_gate.common,
	&jpg0_axi_gate.common,
	&gpg1_axi_gate.common,
	&isp0_axi_gate.common,
	&isp1_axi_gate.common,
	&isp2_axi_gate.common,
	&cpp_axi_gate.common,
	&d0_if_axi_gate.common,
	&d2i_if_axi_gate.common,
	&i2d_if_axi_gate.common,
	&spare_axi_gate.common,
	&sensor2_gate.common,
	&d0if_in_d_en.common,
	&d1if_in_d_en.common,
	&d0if_in_d2i_en.common,
	&d1if_in_d2i_en.common,
	&ia_in_d2i_en.common,
	&ib_in_d2i_en.common,
	&ic_in_d2i_en.common,
	&ia_in_i_en.common,
	&ib_in_i_en.common,
	&ic_in_i_en.common,
	&dispc0_eb.common,
	&dispc1_eb.common,
	&dispc_mmu_eb.common,
	&gsp0_eb.common,
	&gsp1_eb.common,
	&gsp0_mmu_eb.common,
	&gsp1_mmu_eb.common,
	&dsi0_eb.common,
	&dsi1_eb.common,
	&disp_ckg_eb.common,
	&disp_gpu_eb.common,
	&gpu_mtx_eb.common,
	&gsp_mtx_eb.common,
	&tmc_mtx_eb.common,
	&dispc_mtx_eb.common,
	&dphy0_gate.common,
	&dphy1_gate.common,
	&gsp0_a_gate.common,
	&gsp1_a_gate.common,
	&gsp0_f_gate.common,
	&gsp1_f_gate.common,
	&d_mtx_f_gate.common,
	&d_mtx_a_gate.common,
	&d_noc_f_gate.common,
	&d_noc_a_gate.common,
	&gsp_mtx_f_gate.common,
	&gsp_mtx_a_gate.common,
	&gsp_noc_f_gate.common,
	&gsp_noc_a_gate.common,
	&dispm0idle_gate.common,
	&gspm0idle_gate.common,
	&sim0_eb.common,
	&iis0_eb.common,
	&iis1_eb.common,
	&iis2_eb.common,
	&iis3_eb.common,
	&spi0_eb.common,
	&spi1_eb.common,
	&spi2_eb.common,
	&i2c0_eb.common,
	&i2c1_eb.common,
	&i2c2_eb.common,
	&i2c3_eb.common,
	&i2c4_eb.common,
	&i2c5_eb.common,
	&uart0_eb.common,
	&uart1_eb.common,
	&uart2_eb.common,
	&uart3_eb.common,
	&uart4_eb.common,
	&ap_ckg_eb.common,
	&spi3_eb.common,
};

static struct clk_hw_onecell_data sc9860_hw_clks = {
	.hws	= {
		[CLK_FAC_4M]		= &fac_4m.hw,
		[CLK_FAC_2M]		= &fac_2m.hw,
		[CLK_FAC_1M]		= &fac_1m.hw,
		[CLK_FAC_250K]		= &fac_250k.hw,
		[CLK_FAC_RPLL0_26M]	= &fac_rpll0_26m.hw,
		[CLK_FAC_RPLL1_26M]	= &fac_rpll1_26m.hw,
		[CLK_FAC_RCO25M]	= &fac_rco_25m.hw,
		[CLK_FAC_RCO4M]		= &fac_rco_4m.hw,
		[CLK_FAC_RCO2M]		= &fac_rco_2m.hw,
		[CLK_FAC_3K2]		= &fac_3k2.hw,
		[CLK_FAC_1K]		= &fac_1k.hw,
		[CLK_RPLL0_GATE]	= &rpll0_gate.common.hw,
		[CLK_RPLL1_GATE]	= &rpll1_gate.common.hw,
		[CLK_MPLL0_GATE]	= &mpll0_gate.common.hw,
		[CLK_MPLL1_GATE]	= &mpll1_gate.common.hw,
		[CLK_DPLL0_GATE]	= &dpll0_gate.common.hw,
		[CLK_DPLL1_GATE]	= &dpll1_gate.common.hw,
		[CLK_GPLL_GATE]		= &gpll_gate.common.hw,
		[CLK_CPPLL_GATE]	= &cppll_gate.common.hw,
		[CLK_LTEPLL0_GATE]	= &ltepll0_gate.common.hw,
		[CLK_LTEPLL1_GATE]	= &ltepll1_gate.common.hw,
		[CLK_TWPLL_GATE]	= &twpll_gate.common.hw,
		[CLK_SDIO0_2X_EN]	= &sdio0_2x_en.common.hw,
		[CLK_SDIO0_1X_EN]	= &sdio0_1x_en.common.hw,
		[CLK_SDIO1_2X_EN]	= &sdio1_2x_en.common.hw,
		[CLK_SDIO1_1X_EN]	= &sdio1_1x_en.common.hw,
		[CLK_SDIO2_2X_EN]	= &sdio2_2x_en.common.hw,
		[CLK_SDIO2_1X_EN]	= &sdio2_1x_en.common.hw,
		[CLK_EMMC_1X_EN]	= &emmc_1x_en.common.hw,
		[CLK_EMMC_2X_EN]	= &emmc_2x_en.common.hw,
		[CLK_RPLL0]		= &rpll0_clk.common.hw,
		[CLK_RPLL1]		= &rpll1_clk.common.hw,
		[CLK_MPLL0]		= &mpll0_clk.common.hw,
		[CLK_MPLL1]		= &mpll1_clk.common.hw,
		[CLK_DPLL0]		= &dpll0_clk.common.hw,
		[CLK_DPLL1]		= &dpll1_clk.common.hw,
		[CLK_GPLL]		= &gpll_clk.common.hw,
		[CLK_CPPLL]		= &cppll_clk.common.hw,
		[CLK_LTEPLL0]		= &ltepll0_clk.common.hw,
		[CLK_LTEPLL1]		= &ltepll1_clk.common.hw,
		[CLK_TWPLL]		= &twpll_clk.common.hw,
		[CLK_GPLL_42M5]		= &gpll_42m5.hw,
		[CLK_TWPLL_768M]	= &twpll_768m.hw,
		[CLK_TWPLL_384M]	= &twpll_384m.hw,
		[CLK_TWPLL_192M]	= &twpll_192m.hw,
		[CLK_TWPLL_96M]		= &twpll_96m.hw,
		[CLK_TWPLL_48M]		= &twpll_48m.hw,
		[CLK_TWPLL_24M]		= &twpll_24m.hw,
		[CLK_TWPLL_12M]		= &twpll_12m.hw,
		[CLK_TWPLL_512M]	= &twpll_512m.hw,
		[CLK_TWPLL_256M]	= &twpll_256m.hw,
		[CLK_TWPLL_128M]	= &twpll_128m.hw,
		[CLK_TWPLL_64M]		= &twpll_64m.hw,
		[CLK_TWPLL_307M2]	= &twpll_307m2.hw,
		[CLK_TWPLL_153M6]	= &twpll_153m6.hw,
		[CLK_TWPLL_76M8]	= &twpll_76m8.hw,
		[CLK_TWPLL_51M2]	= &twpll_51m2.hw,
		[CLK_TWPLL_38M4]	= &twpll_38m4.hw,
		[CLK_TWPLL_19M2]	= &twpll_19m2.hw,
		[CLK_L0_614M4]		= &l0_614m4.hw,
		[CLK_L0_409M6]		= &l0_409m6.hw,
		[CLK_L0_38M]		= &l0_38m.hw,
		[CLK_L1_38M]		= &l1_38m.hw,
		[CLK_RPLL0_192M]	= &rpll0_192m.hw,
		[CLK_RPLL0_96M]		= &rpll0_96m.hw,
		[CLK_RPLL0_48M]		= &rpll0_48m.hw,
		[CLK_RPLL1_468M]	= &rpll1_468m.hw,
		[CLK_RPLL1_192M]	= &rpll1_192m.hw,
		[CLK_RPLL1_96M]		= &rpll1_96m.hw,
		[CLK_RPLL1_64M]		= &rpll1_64m.hw,
		[CLK_RPLL1_48M]		= &rpll1_48m.hw,
		[CLK_DPLL0_50M]		= &dpll0_50m.hw,
		[CLK_DPLL1_50M]		= &dpll1_50m.hw,
		[CLK_CPPLL_50M]		= &cppll_50m.hw,
		[CLK_M0_39M]		= &m0_39m.hw,
		[CLK_M1_63M]		= &m1_63m.hw,
		[CLK_AON_APB]		= &aon_apb.common.hw,
		[CLK_AUX0]		= &aux0_clk.common.hw,
		[CLK_AUX1]		= &aux1_clk.common.hw,
		[CLK_AUX2]		= &aux2_clk.common.hw,
		[CLK_PROBE]		= &probe_clk.common.hw,
		[CLK_SP_AHB]		= &sp_ahb.common.hw,
		[CLK_CCI]		= &cci_clk.common.hw,
		[CLK_GIC]		= &gic_clk.common.hw,
		[CLK_CSSYS]		= &cssys_clk.common.hw,
		[CLK_SDIO0_2X]		= &sdio0_2x.common.hw,
		[CLK_SDIO1_2X]		= &sdio1_2x.common.hw,
		[CLK_SDIO2_2X]		= &sdio2_2x.common.hw,
		[CLK_EMMC_2X]		= &emmc_2x.common.hw,
		[CLK_UART0]		= &uart0_clk.common.hw,
		[CLK_UART1]		= &uart1_clk.common.hw,
		[CLK_UART2]		= &uart2_clk.common.hw,
		[CLK_UART3]		= &uart3_clk.common.hw,
		[CLK_UART4]		= &uart4_clk.common.hw,
		[CLK_I2C0]		= &i2c0_clk.common.hw,
		[CLK_I2C1]		= &i2c1_clk.common.hw,
		[CLK_I2C2]		= &i2c2_clk.common.hw,
		[CLK_I2C3]		= &i2c3_clk.common.hw,
		[CLK_I2C4]		= &i2c4_clk.common.hw,
		[CLK_I2C5]		= &i2c5_clk.common.hw,
		[CLK_SPI0]		= &spi0_clk.common.hw,
		[CLK_SPI1]		= &spi1_clk.common.hw,
		[CLK_SPI2]		= &spi2_clk.common.hw,
		[CLK_SPI3]		= &spi3_clk.common.hw,
		[CLK_IIS0]		= &iis0_clk.common.hw,
		[CLK_IIS1]		= &iis1_clk.common.hw,
		[CLK_IIS2]		= &iis2_clk.common.hw,
		[CLK_IIS3]		= &iis3_clk.common.hw,
		[CLK_LIT_MCU]		= &lit_mcu.common.hw,
		[CLK_BIG_MCU]		= &big_mcu.common.hw,
		[CLK_GPU]		= &gpu_clk.common.hw,
		[CLK_VSP]		= &vsp_clk.common.hw,
		[CLK_VSP_ENC]		= &vsp_enc.common.hw,
		[CLK_DISPC0_DPI]	= &dispc0_dpi.common.hw,
		[CLK_DISPC1_DPI]	= &dispc1_dpi.common.hw,
		[CLK_SENSOR0]		= &sensor0_clk.common.hw,
		[CLK_SENSOR1]		= &sensor1_clk.common.hw,
		[CLK_SENSOR2]		= &sensor2_clk.common.hw,
		[CLK_SDIO0_1X]		= &sdio0_1x.common.hw,
		[CLK_SDIO1_1X]		= &sdio1_1x.common.hw,
		[CLK_SDIO2_1X]		= &sdio2_1x.common.hw,
		[CLK_EMMC_1X]		= &emmc_1x.common.hw,
		[CLK_ADI]		= &adi_clk.common.hw,
		[CLK_PWM0]		= &pwm0_clk.common.hw,
		[CLK_PWM1]		= &pwm1_clk.common.hw,
		[CLK_PWM2]		= &pwm2_clk.common.hw,
		[CLK_PWM3]		= &pwm3_clk.common.hw,
		[CLK_EFUSE]		= &efuse_clk.common.hw,
		[CLK_CM3_UART0]		= &cm3_uart0.common.hw,
		[CLK_CM3_UART1]		= &cm3_uart1.common.hw,
		[CLK_THM]		= &thm_clk.common.hw,
		[CLK_CM3_I2C0]		= &cm3_i2c0.common.hw,
		[CLK_CM3_I2C1]		= &cm3_i2c1.common.hw,
		[CLK_CM4_SPI]		= &cm4_spi.common.hw,
		[CLK_AON_I2C]		= &aon_i2c.common.hw,
		[CLK_AVS]		= &avs_clk.common.hw,
		[CLK_CA53_DAP]		= &ca53_dap.common.hw,
		[CLK_CA53_TS]		= &ca53_ts.common.hw,
		[CLK_DJTAG_TCK]		= &djtag_tck.common.hw,
		[CLK_PMU]		= &pmu_clk.common.hw,
		[CLK_PMU_26M]		= &pmu_26m.common.hw,
		[CLK_DEBOUNCE]		= &debounce_clk.common.hw,
		[CLK_OTG2_REF]		= &otg2_ref.common.hw,
		[CLK_USB3_REF]		= &usb3_ref.common.hw,
		[CLK_AP_AXI]		= &ap_axi.common.hw,
		[CLK_AP_APB]		= &ap_apb.common.hw,
		[CLK_AHB_VSP]		= &ahb_vsp.common.hw,
		[CLK_AHB_DISP]		= &ahb_disp.common.hw,
		[CLK_AHB_CAM]		= &ahb_cam.common.hw,
		[CLK_MIPI_CSI0_EB]	= &mipi_csi0_eb.common.hw,
		[CLK_MIPI_CSI1_EB]	= &mipi_csi1_eb.common.hw,
		[CLK_USB3_EB]		= &usb3_eb.common.hw,
		[CLK_USB3_SUSPEND_EB]	= &usb3_suspend.common.hw,
		[CLK_USB3_REF_EB]	= &usb3_ref_eb.common.hw,
		[CLK_DMA_EB]		= &dma_eb.common.hw,
		[CLK_SDIO0_EB]		= &sdio0_eb.common.hw,
		[CLK_SDIO1_EB]		= &sdio1_eb.common.hw,
		[CLK_SDIO2_EB]		= &sdio2_eb.common.hw,
		[CLK_EMMC_EB]		= &emmc_eb.common.hw,
		[CLK_ROM_EB]		= &rom_eb.common.hw,
		[CLK_BUSMON_EB]		= &busmon_eb.common.hw,
		[CLK_CC63S_EB]		= &cc63s_eb.common.hw,
		[CLK_CC63P_EB]		= &cc63p_eb.common.hw,
		[CLK_CE0_EB]		= &ce0_eb.common.hw,
		[CLK_CE1_EB]		= &ce1_eb.common.hw,
		[CLK_AVS_LIT_EB]	= &avs_lit_eb.common.hw,
		[CLK_AVS_BIG_EB]	= &avs_big_eb.common.hw,
		[CLK_AP_INTC5_EB]	= &ap_intc5_eb.common.hw,
		[CLK_GPIO_EB]		= &gpio_eb.common.hw,
		[CLK_PWM0_EB]		= &pwm0_eb.common.hw,
		[CLK_PWM1_EB]		= &pwm1_eb.common.hw,
		[CLK_PWM2_EB]		= &pwm2_eb.common.hw,
		[CLK_PWM3_EB]		= &pwm3_eb.common.hw,
		[CLK_KPD_EB]		= &kpd_eb.common.hw,
		[CLK_AON_SYS_EB]	= &aon_sys_eb.common.hw,
		[CLK_AP_SYS_EB]		= &ap_sys_eb.common.hw,
		[CLK_AON_TMR_EB]	= &aon_tmr_eb.common.hw,
		[CLK_AP_TMR0_EB]	= &ap_tmr0_eb.common.hw,
		[CLK_EFUSE_EB]		= &efuse_eb.common.hw,
		[CLK_EIC_EB]		= &eic_eb.common.hw,
		[CLK_PUB1_REG_EB]	= &pub1_reg_eb.common.hw,
		[CLK_ADI_EB]		= &adi_eb.common.hw,
		[CLK_AP_INTC0_EB]	= &ap_intc0_eb.common.hw,
		[CLK_AP_INTC1_EB]	= &ap_intc1_eb.common.hw,
		[CLK_AP_INTC2_EB]	= &ap_intc2_eb.common.hw,
		[CLK_AP_INTC3_EB]	= &ap_intc3_eb.common.hw,
		[CLK_AP_INTC4_EB]	= &ap_intc4_eb.common.hw,
		[CLK_SPLK_EB]		= &splk_eb.common.hw,
		[CLK_MSPI_EB]		= &mspi_eb.common.hw,
		[CLK_PUB0_REG_EB]	= &pub0_reg_eb.common.hw,
		[CLK_PIN_EB]		= &pin_eb.common.hw,
		[CLK_AON_CKG_EB]	= &aon_ckg_eb.common.hw,
		[CLK_GPU_EB]		= &gpu_eb.common.hw,
		[CLK_APCPU_TS0_EB]	= &apcpu_ts0_eb.common.hw,
		[CLK_APCPU_TS1_EB]	= &apcpu_ts1_eb.common.hw,
		[CLK_DAP_EB]		= &dap_eb.common.hw,
		[CLK_I2C_EB]		= &i2c_eb.common.hw,
		[CLK_PMU_EB]		= &pmu_eb.common.hw,
		[CLK_THM_EB]		= &thm_eb.common.hw,
		[CLK_AUX0_EB]		= &aux0_eb.common.hw,
		[CLK_AUX1_EB]		= &aux1_eb.common.hw,
		[CLK_AUX2_EB]		= &aux2_eb.common.hw,
		[CLK_PROBE_EB]		= &probe_eb.common.hw,
		[CLK_GPU0_AVS_EB]	= &gpu0_avs_eb.common.hw,
		[CLK_GPU1_AVS_EB]	= &gpu1_avs_eb.common.hw,
		[CLK_APCPU_WDG_EB]	= &apcpu_wdg_eb.common.hw,
		[CLK_AP_TMR1_EB]	= &ap_tmr1_eb.common.hw,
		[CLK_AP_TMR2_EB]	= &ap_tmr2_eb.common.hw,
		[CLK_DISP_EMC_EB]	= &disp_emc_eb.common.hw,
		[CLK_ZIP_EMC_EB]	= &zip_emc_eb.common.hw,
		[CLK_GSP_EMC_EB]	= &gsp_emc_eb.common.hw,
		[CLK_OSC_AON_EB]	= &osc_aon_eb.common.hw,
		[CLK_LVDS_TRX_EB]	= &lvds_trx_eb.common.hw,
		[CLK_LVDS_TCXO_EB]	= &lvds_tcxo_eb.common.hw,
		[CLK_MDAR_EB]		= &mdar_eb.common.hw,
		[CLK_RTC4M0_CAL_EB]	= &rtc4m0_cal_eb.common.hw,
		[CLK_RCT100M_CAL_EB]	= &rct100m_cal_eb.common.hw,
		[CLK_DJTAG_EB]		= &djtag_eb.common.hw,
		[CLK_MBOX_EB]		= &mbox_eb.common.hw,
		[CLK_AON_DMA_EB]	= &aon_dma_eb.common.hw,
		[CLK_DBG_EMC_EB]	= &dbg_emc_eb.common.hw,
		[CLK_LVDS_PLL_DIV_EN]	= &lvds_pll_div_en.common.hw,
		[CLK_DEF_EB]		= &def_eb.common.hw,
		[CLK_AON_APB_RSV0]	= &aon_apb_rsv0.common.hw,
		[CLK_ORP_JTAG_EB]	= &orp_jtag_eb.common.hw,
		[CLK_VSP_EB]		= &vsp_eb.common.hw,
		[CLK_CAM_EB]		= &cam_eb.common.hw,
		[CLK_DISP_EB]		= &disp_eb.common.hw,
		[CLK_DBG_AXI_IF_EB]	= &dbg_axi_if_eb.common.hw,
		[CLK_AGCP_IIS0_EB]	= &agcp_iis0_eb.common.hw,
		[CLK_AGCP_IIS1_EB]	= &agcp_iis1_eb.common.hw,
		[CLK_AGCP_IIS2_EB]	= &agcp_iis2_eb.common.hw,
		[CLK_AGCP_IIS3_EB]	= &agcp_iis3_eb.common.hw,
		[CLK_AGCP_UART_EB]	= &agcp_uart_eb.common.hw,
		[CLK_AGCP_DMACP_EB]	= &agcp_dmacp_eb.common.hw,
		[CLK_AGCP_DMAAP_EB]	= &agcp_dmaap_eb.common.hw,
		[CLK_AGCP_ARC48K_EB]	= &agcp_arc48k_eb.common.hw,
		[CLK_AGCP_SRC44P1K_EB]	= &agcp_src44p1k_eb.common.hw,
		[CLK_AGCP_MCDT_EB]	= &agcp_mcdt_eb.common.hw,
		[CLK_AGCP_VBCIFD_EB]	= &agcp_vbcifd_eb.common.hw,
		[CLK_AGCP_VBC_EB]	= &agcp_vbc_eb.common.hw,
		[CLK_AGCP_SPINLOCK_EB]	= &agcp_spinlock_eb.common.hw,
		[CLK_AGCP_ICU_EB]	= &agcp_icu_eb.common.hw,
		[CLK_AGCP_AP_ASHB_EB]	= &agcp_ap_ashb_eb.common.hw,
		[CLK_AGCP_CP_ASHB_EB]	= &agcp_cp_ashb_eb.common.hw,
		[CLK_AGCP_AUD_EB]	= &agcp_aud_eb.common.hw,
		[CLK_AGCP_AUDIF_EB]	= &agcp_audif_eb.common.hw,
		[CLK_VSP_DEC_EB]	= &vsp_dec_eb.common.hw,
		[CLK_VSP_CKG_EB]	= &vsp_ckg_eb.common.hw,
		[CLK_VSP_MMU_EB]	= &vsp_mmu_eb.common.hw,
		[CLK_VSP_ENC_EB]	= &vsp_enc_eb.common.hw,
		[CLK_VPP_EB]		= &vpp_eb.common.hw,
		[CLK_VSP_26M_EB]	= &vsp_26m_eb.common.hw,
		[CLK_VSP_AXI_GATE]	= &vsp_axi_gate.common.hw,
		[CLK_VSP_ENC_GATE]	= &vsp_enc_gate.common.hw,
		[CLK_VPP_AXI_GATE]	= &vpp_axi_gate.common.hw,
		[CLK_VSP_BM_GATE]	= &vsp_bm_gate.common.hw,
		[CLK_VSP_ENC_BM_GATE]	= &vsp_enc_bm_gate.common.hw,
		[CLK_VPP_BM_GATE]	= &vpp_bm_gate.common.hw,
		[CLK_DCAM0_EB]		= &dcam0_eb.common.hw,
		[CLK_DCAM1_EB]		= &dcam1_eb.common.hw,
		[CLK_ISP0_EB]		= &isp0_eb.common.hw,
		[CLK_CSI0_EB]		= &csi0_eb.common.hw,
		[CLK_CSI1_EB]		= &csi1_eb.common.hw,
		[CLK_JPG0_EB]		= &jpg0_eb.common.hw,
		[CLK_JPG1_EB]		= &jpg1_eb.common.hw,
		[CLK_CAM_CKG_EB]	= &cam_ckg_eb.common.hw,
		[CLK_CAM_MMU_EB]	= &cam_mmu_eb.common.hw,
		[CLK_ISP1_EB]		= &isp1_eb.common.hw,
		[CLK_CPP_EB]		= &cpp_eb.common.hw,
		[CLK_MMU_PF_EB]		= &mmu_pf_eb.common.hw,
		[CLK_ISP2_EB]		= &isp2_eb.common.hw,
		[CLK_DCAM2ISP_IF_EB]	= &dcam2isp_if_eb.common.hw,
		[CLK_ISP2DCAM_IF_EB]	= &isp2dcam_if_eb.common.hw,
		[CLK_ISP_LCLK_EB]	= &isp_lclk_eb.common.hw,
		[CLK_ISP_ICLK_EB]	= &isp_iclk_eb.common.hw,
		[CLK_ISP_MCLK_EB]	= &isp_mclk_eb.common.hw,
		[CLK_ISP_PCLK_EB]	= &isp_pclk_eb.common.hw,
		[CLK_ISP_ISP2DCAM_EB]	= &isp_isp2dcam_eb.common.hw,
		[CLK_DCAM0_IF_EB]	= &dcam0_if_eb.common.hw,
		[CLK_CLK26M_IF_EB]	= &clk26m_if_eb.common.hw,
		[CLK_CPHY0_GATE]	= &cphy0_gate.common.hw,
		[CLK_MIPI_CSI0_GATE]	= &mipi_csi0_gate.common.hw,
		[CLK_CPHY1_GATE]	= &cphy1_gate.common.hw,
		[CLK_MIPI_CSI1]		= &mipi_csi1.common.hw,
		[CLK_DCAM0_AXI_GATE]	= &dcam0_axi_gate.common.hw,
		[CLK_DCAM1_AXI_GATE]	= &dcam1_axi_gate.common.hw,
		[CLK_SENSOR0_GATE]	= &sensor0_gate.common.hw,
		[CLK_SENSOR1_GATE]	= &sensor1_gate.common.hw,
		[CLK_JPG0_AXI_GATE]	= &jpg0_axi_gate.common.hw,
		[CLK_GPG1_AXI_GATE]	= &gpg1_axi_gate.common.hw,
		[CLK_ISP0_AXI_GATE]	= &isp0_axi_gate.common.hw,
		[CLK_ISP1_AXI_GATE]	= &isp1_axi_gate.common.hw,
		[CLK_ISP2_AXI_GATE]	= &isp2_axi_gate.common.hw,
		[CLK_CPP_AXI_GATE]	= &cpp_axi_gate.common.hw,
		[CLK_D0_IF_AXI_GATE]	= &d0_if_axi_gate.common.hw,
		[CLK_D2I_IF_AXI_GATE]	= &d2i_if_axi_gate.common.hw,
		[CLK_I2D_IF_AXI_GATE]	= &i2d_if_axi_gate.common.hw,
		[CLK_SPARE_AXI_GATE]	= &spare_axi_gate.common.hw,
		[CLK_SENSOR2_GATE]	= &sensor2_gate.common.hw,
		[CLK_D0IF_IN_D_EN]	= &d0if_in_d_en.common.hw,
		[CLK_D1IF_IN_D_EN]	= &d1if_in_d_en.common.hw,
		[CLK_D0IF_IN_D2I_EN]	= &d0if_in_d2i_en.common.hw,
		[CLK_D1IF_IN_D2I_EN]	= &d1if_in_d2i_en.common.hw,
		[CLK_IA_IN_D2I_EN]	= &ia_in_d2i_en.common.hw,
		[CLK_IB_IN_D2I_EN]	= &ib_in_d2i_en.common.hw,
		[CLK_IC_IN_D2I_EN]	= &ic_in_d2i_en.common.hw,
		[CLK_IA_IN_I_EN]	= &ia_in_i_en.common.hw,
		[CLK_IB_IN_I_EN]	= &ib_in_i_en.common.hw,
		[CLK_IC_IN_I_EN]	= &ic_in_i_en.common.hw,
		[CLK_DISPC0_EB]		= &dispc0_eb.common.hw,
		[CLK_DISPC1_EB]		= &dispc1_eb.common.hw,
		[CLK_DISPC_MMU_EB]	= &dispc_mmu_eb.common.hw,
		[CLK_GSP0_EB]		= &gsp0_eb.common.hw,
		[CLK_GSP1_EB]		= &gsp1_eb.common.hw,
		[CLK_GSP0_MMU_EB]	= &gsp0_mmu_eb.common.hw,
		[CLK_GSP1_MMU_EB]	= &gsp1_mmu_eb.common.hw,
		[CLK_DSI0_EB]		= &dsi0_eb.common.hw,
		[CLK_DSI1_EB]		= &dsi1_eb.common.hw,
		[CLK_DISP_CKG_EB]	= &disp_ckg_eb.common.hw,
		[CLK_DISP_GPU_EB]	= &disp_gpu_eb.common.hw,
		[CLK_GPU_MTX_EB]	= &gpu_mtx_eb.common.hw,
		[CLK_GSP_MTX_EB]	= &gsp_mtx_eb.common.hw,
		[CLK_TMC_MTX_EB]	= &tmc_mtx_eb.common.hw,
		[CLK_DISPC_MTX_EB]	= &dispc_mtx_eb.common.hw,
		[CLK_DPHY0_GATE]	= &dphy0_gate.common.hw,
		[CLK_DPHY1_GATE]	= &dphy1_gate.common.hw,
		[CLK_GSP0_A_GATE]	= &gsp0_a_gate.common.hw,
		[CLK_GSP1_A_GATE]	= &gsp1_a_gate.common.hw,
		[CLK_GSP0_F_GATE]	= &gsp0_f_gate.common.hw,
		[CLK_GSP1_F_GATE]	= &gsp1_f_gate.common.hw,
		[CLK_D_MTX_F_GATE]	= &d_mtx_f_gate.common.hw,
		[CLK_D_MTX_A_GATE]	= &d_mtx_a_gate.common.hw,
		[CLK_D_NOC_F_GATE]	= &d_noc_f_gate.common.hw,
		[CLK_D_NOC_A_GATE]	= &d_noc_a_gate.common.hw,
		[CLK_GSP_MTX_F_GATE]	= &gsp_mtx_f_gate.common.hw,
		[CLK_GSP_MTX_A_GATE]	= &gsp_mtx_a_gate.common.hw,
		[CLK_GSP_NOC_F_GATE]	= &gsp_noc_f_gate.common.hw,
		[CLK_GSP_NOC_A_GATE]	= &gsp_noc_a_gate.common.hw,
		[CLK_DISPM0IDLE_GATE]	= &dispm0idle_gate.common.hw,
		[CLK_GSPM0IDLE_GATE]	= &gspm0idle_gate.common.hw,
		[CLK_SIM0_EB]		= &sim0_eb.common.hw,
		[CLK_IIS0_EB]		= &iis0_eb.common.hw,
		[CLK_IIS1_EB]		= &iis1_eb.common.hw,
		[CLK_IIS2_EB]		= &iis2_eb.common.hw,
		[CLK_IIS3_EB]		= &iis3_eb.common.hw,
		[CLK_SPI0_EB]		= &spi0_eb.common.hw,
		[CLK_SPI1_EB]		= &spi1_eb.common.hw,
		[CLK_SPI2_EB]		= &spi2_eb.common.hw,
		[CLK_I2C0_EB]		= &i2c0_eb.common.hw,
		[CLK_I2C1_EB]		= &i2c1_eb.common.hw,
		[CLK_I2C2_EB]		= &i2c2_eb.common.hw,
		[CLK_I2C3_EB]		= &i2c3_eb.common.hw,
		[CLK_I2C4_EB]		= &i2c4_eb.common.hw,
		[CLK_I2C5_EB]		= &i2c5_eb.common.hw,
		[CLK_UART0_EB]		= &uart0_eb.common.hw,
		[CLK_UART1_EB]		= &uart1_eb.common.hw,
		[CLK_UART2_EB]		= &uart2_eb.common.hw,
		[CLK_UART3_EB]		= &uart3_eb.common.hw,
		[CLK_UART4_EB]		= &uart4_eb.common.hw,
		[CLK_AP_CKG_EB]		= &ap_ckg_eb.common.hw,
		[CLK_SPI3_EB]		= &spi3_eb.common.hw,
	},
	.num	= CLK_NUMBER_SC9860,
};

static const struct sprd_ccu_desc sc9860_ccu_desc = {
	.ccu_clks	= sc9860_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sc9860_ccu_clks),

	.hw_clks	= &sc9860_hw_clks,
};

static void __init sc9860_ccu_init(struct device_node *node,
					const struct sprd_ccu_desc *desc)
{
	void __iomem *base;
	int i, count;
	struct resource res;
	struct ccu_addr_map *sc9860_maps;

	count = of_property_count_u64_elems(node, "reg");
	if (count <= 0) {
		pr_err("%s: no reg properties found for %s\n",
		       __func__, of_node_full_name(node));
		return;
	}
	count = count / 2;

	sc9860_maps = kcalloc(count, sizeof(*sc9860_maps), GFP_KERNEL);
	if (!sc9860_maps)
		return;

	for (i = 0; i < count; i++) {
		if (of_address_to_resource(node, i, &res)) {
			pr_err("%s: wrong reg[%d] found for %s\n",
			       __func__, i, of_node_full_name(node));
			goto unmap_maps;
		}

		base = ioremap(res.start, resource_size(&res));
		if (IS_ERR(base)) {
			pr_err("%s: clock[%s] ioremap failed!\n",
			       __func__, node->full_name);
			goto unmap_maps;
		}

		sc9860_maps[i].phy = res.start & 0xffff0000;
		sc9860_maps[i].virt = base;
	}

	if (sprd_ccu_probe(node, sc9860_maps, count, desc))
		goto unmap_maps;


	kfree(sc9860_maps);
	pr_info("%u SC9860 clocks have been registered now.\n",
		CLK_NUMBER_SC9860);
	return;

unmap_maps:
	while (--i >= 0)
		iounmap(sc9860_maps[i].virt);
	kfree(sc9860_maps);
}

static void __init sc9860_ccu_setup(struct device_node *node)
{
	sc9860_ccu_init(node, &sc9860_ccu_desc);
}
CLK_OF_DECLARE(sc9860_ccu, "sprd,sc9860-ccu", sc9860_ccu_setup);
