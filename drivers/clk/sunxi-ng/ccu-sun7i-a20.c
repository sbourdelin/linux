/*
 * Copyright (c) 2017 Priit Laes. All rights reserved.
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

#include <linux/clk-provider.h>
#include <linux/of_address.h>

#include "ccu_common.h"
#include "ccu_reset.h"

#include "ccu_div.h"
#include "ccu_gate.h"
#include "ccu_mp.h"
#include "ccu_mult.h"
#include "ccu_nk.h"
#include "ccu_nkm.h"
#include "ccu_nkmp.h"
#include "ccu_nm.h"
#include "ccu_phase.h"

#include "ccu-sun7i-a20.h"

/*
 * PLL1 - Core clock
 *
 * TODO: sigma-delta pattern bits 2 & 3
 * TODO: PLL1 tuning register
 */
static struct ccu_nkmp pll_core_clk = {
	.enable		= BIT(31),
	.n		= _SUNXI_CCU_MULT_OFFSET(8, 5, 0),
	.k		= _SUNXI_CCU_MULT(4, 2),
	.m		= _SUNXI_CCU_DIV(0, 2),
	.p		= _SUNXI_CCU_DIV(16, 2),
	.common		= {
		.reg		= 0x000,
		.hw.init	= CLK_HW_INIT("pll-core",
					      "hosc",
					      &ccu_nkmp_ops,
					      0),
	},
};

/* PLL2 - Audio clock */
static struct ccu_nm pll_audio_base_clk = {
	.enable		= BIT(31),
	.n		= _SUNXI_CCU_MULT_OFFSET(8, 7, 0),
	.m		= _SUNXI_CCU_DIV_OFFSET(0, 5, 0),
	.common		= {
		.reg		= 0x008,
		.hw.init	= CLK_HW_INIT("pll-audio-base",
					      "hosc",
					      &ccu_nm_ops,
					      0),
	},

};

/* PLL3 - Video0 clock */
static struct ccu_mult pll_video0_clk = {
	.enable		= BIT(31),
	.mult		= _SUNXI_CCU_MULT_OFFSET_MIN_MAX(0, 7, 0, 9, 127),
	.frac		= _SUNXI_CCU_FRAC(BIT(15), BIT(14),
					  270000000, 297000000),
	.common		= {
		.reg		= 0x010,
		.features	= (CCU_FEATURE_FRACTIONAL |
				   CCU_FEATURE_ALL_PREDIV),
		.prediv		= 8,
		.hw.init	= CLK_HW_INIT("pll-video0",
					      "hosc",
					      &ccu_mult_ops,
					      0),
	},
};

/* PLL4 - VE clock */
static struct ccu_nkmp pll_ve_clk = {
	.enable		= BIT(31),
	.n		= _SUNXI_CCU_MULT_OFFSET(8, 5, 0),
	.k		= _SUNXI_CCU_MULT(4, 2),
	.m		= _SUNXI_CCU_DIV(0, 2),
	.p		= _SUNXI_CCU_DIV(16, 2),
	.common		= {
		.reg		= 0x018,
		.hw.init	= CLK_HW_INIT("pll-ve",
					      "hosc",
					      &ccu_nkmp_ops,
					      0),
	},
};

/*
 * PLL5 - DDR clock
 *
 * TODO: PLL5 tuning register
 */
static struct ccu_nk pll_ddr_base_clk = {
	.enable		= BIT(31),
	.n		= _SUNXI_CCU_MULT_OFFSET(8, 5, 0),
	.k		= _SUNXI_CCU_MULT(4, 2),
	.common		= {
		.reg		= 0x020,
		.hw.init	= CLK_HW_INIT("pll-ddr-base",
					      "hosc",
					      &ccu_nk_ops,
					      0),
	},
};

static SUNXI_CCU_M(pll_ddr_clk, "pll-ddr", "pll-ddr-base", 0x020, 0, 2,
		   CLK_IS_CRITICAL);

static struct ccu_div pll_ddr_other_clk = {
	.div		= _SUNXI_CCU_DIV_FLAGS(16, 2, CLK_DIVIDER_POWER_OF_TWO),

	.common		= {
		.reg		= 0x020,
		.hw.init	= CLK_HW_INIT("pll-ddr-other", "pll-ddr-base",
					      &ccu_div_ops,
					      0),
	},
};

/* PLL6 - peripheral (SATA) clock */
static struct ccu_nk pll_periph_clk = {
	.enable		= BIT(31),
	.n		= _SUNXI_CCU_MULT_OFFSET(8, 5, 0),
	.k		= _SUNXI_CCU_MULT(4, 2),
	.fixed_post_div	= 2,
	.common		= {
		.reg		= 0x028,
		.features	= CCU_FEATURE_FIXED_POSTDIV,
		.hw.init	= CLK_HW_INIT("pll-periph",
					      "hosc",
					      &ccu_nk_ops,
					      0),
	},
};
static SUNXI_CCU_GATE(pll_periph_sata_clk, "pll-periph-sata", "pll-periph",
		      0x028, BIT(14), 0);

/* PLL7 - Video1 clock */
static struct ccu_mult pll_video1_clk = {
	.enable		= BIT(31),
	.mult		= _SUNXI_CCU_MULT_OFFSET_MIN_MAX(0, 7, 0, 9, 127),
	.frac		= _SUNXI_CCU_FRAC(BIT(15), BIT(14),
				  270000000, 297000000),
	.common		= {
		.reg		= 0x030,
		.features	= (CCU_FEATURE_FRACTIONAL |
				   CCU_FEATURE_ALL_PREDIV),
		.prediv		= 8,
		.hw.init	= CLK_HW_INIT("pll-video1",
					      "hosc",
					      &ccu_mult_ops,
					      0),
	},
};

/* TODO: pll8 gpu 0x040 */

static SUNXI_CCU_GATE(hosc_clk,	"hosc",	"osc24M", 0x050, BIT(0), 0);

static const char *const cpu_parents[] = { "osc32k", "hosc",
					   "pll-core", "pll-periph" };
static const struct ccu_mux_fixed_prediv cpu_predivs[] = {
	{ .index = 3, .div = 3, },
};

static struct ccu_mux cpu_clk = {
	.mux		= {
		.shift		= 16,
		.width		= 2,
		.fixed_predivs	= cpu_predivs,
		.n_predivs	= ARRAY_SIZE(cpu_predivs),
	},
	.common		= {
		.reg		= 0x054,
		.features	= CCU_FEATURE_FIXED_PREDIV,
		.hw.init	= CLK_HW_INIT_PARENTS("cpu",
						      cpu_parents,
						      &ccu_mux_ops,
						      CLK_IS_CRITICAL),
	}
};

static SUNXI_CCU_M(axi_clk, "axi", "cpu", 0x054, 0, 2, 0);

static const char *const ahb_parents[] = { "axi", "pll-periph",
					   "pll-periph-2x" };
static const struct ccu_mux_fixed_prediv ahb_predivs[] = {
	{ .index = 2, .div = 2, },
};

static struct ccu_div ahb_clk = {
	.div		= _SUNXI_CCU_DIV_FLAGS(4, 2, CLK_DIVIDER_POWER_OF_TWO),
	.mux		= {
		.shift		= 6,
		.width		= 2,
		.fixed_predivs	= ahb_predivs,
		.n_predivs	= ARRAY_SIZE(ahb_predivs),
	},

	.common		= {
		.reg		= 0x054,
		.hw.init	= CLK_HW_INIT_PARENTS("ahb",
						      ahb_parents,
						      &ccu_div_ops,
						      0),
	},
};

static struct clk_div_table apb0_div_table[] = {
	{ .val = 0, .div = 2 },
	{ .val = 1, .div = 2 },
	{ .val = 2, .div = 4 },
	{ .val = 3, .div = 8 },
	{ /* Sentinel */ },
};
static SUNXI_CCU_DIV_TABLE(apb0_clk, "apb0", "ahb",
			   0x054, 8, 2, apb0_div_table, 0);

static const char *const apb1_parents[] = { "hosc", "pll-periph", "osc32k" };
static SUNXI_CCU_MP_WITH_MUX(apb1_clk, "apb1", apb1_parents, 0x058,
			     0, 5,	/* M */
			     16, 2,	/* P */
			     24, 2,	/* mux */
			     0);

static SUNXI_CCU_GATE(ahb_otg_clk,	"ahb-otg",	"ahb",
		      0x060, BIT(0), 0);
static SUNXI_CCU_GATE(ahb_ehci0_clk,	"ahb-ehci0",	"ahb",
		      0x060, BIT(1), 0);
static SUNXI_CCU_GATE(ahb_ohci0_clk,	"ahb-ohci0",	"ahb",
		      0x060, BIT(2), 0);
static SUNXI_CCU_GATE(ahb_ehci1_clk,	"ahb-ehci1",	"ahb",
		      0x060, BIT(3), 0);
static SUNXI_CCU_GATE(ahb_ohci1_clk,	"ahb-ohci1",	"ahb",
		      0x060, BIT(4), 0);
static SUNXI_CCU_GATE(ahb_ss_clk,	"ahb-ss",	"ahb",
		      0x060, BIT(5), 0);
static SUNXI_CCU_GATE(ahb_dma_clk,	"ahb-dma",	"ahb",
		      0x060, BIT(6), 0);
static SUNXI_CCU_GATE(ahb_bist_clk,	"ahb-bist",	"ahb",
		      0x060, BIT(7), 0);
static SUNXI_CCU_GATE(ahb_mmc0_clk,	"ahb-mmc0",	"ahb",
		      0x060, BIT(8), 0);
static SUNXI_CCU_GATE(ahb_mmc1_clk,	"ahb-mmc1",	"ahb",
		      0x060, BIT(9), 0);
static SUNXI_CCU_GATE(ahb_mmc2_clk,	"ahb-mmc2",	"ahb",
		      0x060, BIT(10), 0);
static SUNXI_CCU_GATE(ahb_mmc3_clk,	"ahb-mmc3",	"ahb",
		      0x060, BIT(11), 0);
static SUNXI_CCU_GATE(ahb_ms_clk,	"ahb-ms",	"ahb",
		      0x060, BIT(12), 0);
static SUNXI_CCU_GATE(ahb_nand_clk,	"ahb-nand",	"ahb",
		      0x060, BIT(13), 0);
static SUNXI_CCU_GATE(ahb_sdram_clk,	"ahb-sdram",	"ahb",
		      0x060, BIT(14), CLK_IS_CRITICAL);
/* BIT(15) - reserved */
static SUNXI_CCU_GATE(ahb_ace_clk,	"ahb-ace",	"ahb",
		      0x060, BIT(16), 0);
static SUNXI_CCU_GATE(ahb_emac_clk,	"ahb-emac",	"ahb",
		      0x060, BIT(17), 0);
static SUNXI_CCU_GATE(ahb_ts_clk,	"ahb-ts",	"ahb",
		      0x060, BIT(18), 0);
/* BIT(19) - reserved */
static SUNXI_CCU_GATE(ahb_spi0_clk,	"ahb-spi0",	"ahb",
		      0x060, BIT(20), 0);
static SUNXI_CCU_GATE(ahb_spi1_clk,	"ahb-spi1",	"ahb",
		      0x060, BIT(21), 0);
static SUNXI_CCU_GATE(ahb_spi2_clk,	"ahb-spi2",	"ahb",
		      0x060, BIT(22), 0);
static SUNXI_CCU_GATE(ahb_spi3_clk,	"ahb-spi3",	"ahb",
		      0x060, BIT(23), 0);
/* BIT(24) - reserved */
static SUNXI_CCU_GATE(ahb_sata_clk,	"ahb-sata",	"ahb",
		      0x060, BIT(25), 0);
/* BIT(26 .. 27) - reserved */
static SUNXI_CCU_GATE(ahb_hstimer_clk,	"ahb-hstimer",	"ahb",
		      0x060, BIT(28), 0);
/* BIT(29 .. 31) - reserved */

static SUNXI_CCU_GATE(ahb_ve_clk,	"ahb-ve",	"ahb",
		      0x064, BIT(0), 0);
static SUNXI_CCU_GATE(ahb_tvd_clk,	"ahb-tvd",	"ahb",
		      0x064, BIT(1), 0);
static SUNXI_CCU_GATE(ahb_tve0_clk,	"ahb-tve0",	"ahb",
		      0x064, BIT(2), 0);
static SUNXI_CCU_GATE(ahb_tve1_clk,	"ahb-tve1",	"ahb",
		      0x064, BIT(3), 0);
static SUNXI_CCU_GATE(ahb_lcd0_clk,	"ahb-lcd0",	"ahb",
		      0x064, BIT(4), 0);
static SUNXI_CCU_GATE(ahb_lcd1_clk,	"ahb-lcd1",	"ahb",
		      0x064, BIT(5), 0);
/* BIT(6 .. 7) - reserved */
static SUNXI_CCU_GATE(ahb_csi0_clk,	"ahb-csi0",	"ahb",
		      0x064, BIT(8), 0);
static SUNXI_CCU_GATE(ahb_csi1_clk,	"ahb-csi1",	"ahb",
		      0x064, BIT(9), 0);
static SUNXI_CCU_GATE(ahb_hdmi1_clk,	"ahb-hdmi1",	"ahb",
		      0x064, BIT(10), 0);
static SUNXI_CCU_GATE(ahb_hdmi0_clk,	"ahb-hdmi0",	"ahb",
		      0x064, BIT(11), 0);
static SUNXI_CCU_GATE(ahb_de_be0_clk,	"ahb-de-be0",	"ahb",
		      0x064, BIT(12), 0);
static SUNXI_CCU_GATE(ahb_de_be1_clk,	"ahb-de-be1",	"ahb",
		      0x064, BIT(13), 0);
static SUNXI_CCU_GATE(ahb_de_fe0_clk,	"ahb-de-fe0",	"ahb",
		      0x064, BIT(14), 0);
static SUNXI_CCU_GATE(ahb_de_fe1_clk,	"ahb-de-fe1",	"ahb",
		      0x064, BIT(15), 0);
/* BIT(16) - reserved */
static SUNXI_CCU_GATE(ahb_gmac_clk,	"ahb-gmac",	"ahb",
		      0x064, BIT(17), 0);
static SUNXI_CCU_GATE(ahb_mp_clk,	"ahb-mp",	"ahb",
		      0x064, BIT(18), 0);
/* BIT(19) - reserved */
static SUNXI_CCU_GATE(ahb_gpu_clk,	"ahb-gpu",	"ahb",
		      0x064, BIT(20), 0);
/* BIT(21 .. 31) - reserved */

static SUNXI_CCU_GATE(apb0_codec_clk,	"apb0-codec",	"apb0",
		      0x068, BIT(0), 0);
static SUNXI_CCU_GATE(apb0_spdif_clk,	"apb0-spdif",	"apb0",
		      0x068, BIT(1), 0);
static SUNXI_CCU_GATE(apb0_ac97_clk,	"apb0-ac97",	"apb0",
		      0x068, BIT(2), 0);
static SUNXI_CCU_GATE(apb0_i2s0_clk,	"apb0-i2s0",	"apb0",
		      0x068, BIT(3), 0);
static SUNXI_CCU_GATE(apb0_i2s1_clk,	"apb0-i2s1",	"apb0",
		      0x068, BIT(4), 0);
static SUNXI_CCU_GATE(apb0_pio_clk,	"apb0-pio",	"apb0",
		      0x068, BIT(5), 0);
static SUNXI_CCU_GATE(apb0_ir0_clk,	"apb0-ir0",	"apb0",
		      0x068, BIT(6), 0);
static SUNXI_CCU_GATE(apb0_ir1_clk,	"apb0-ir1",	"apb0",
		      0x068, BIT(7), 0);
static SUNXI_CCU_GATE(apb0_i2s2_clk,	"apb0-i2s2",	"apb0",
		      0x068, BIT(8), 0);
/* BIT(8) - reserved */
static SUNXI_CCU_GATE(apb0_keypad_clk,	"apb0-keypad",	"apb0",
		      0x068, BIT(10), 0);
/* BIT(11 .. 31) - reserved */

static SUNXI_CCU_GATE(apb1_i2c0_clk,	"apb1-i2c0",	"apb1",
		      0x06c, BIT(0), 0);
static SUNXI_CCU_GATE(apb1_i2c1_clk,	"apb1-i2c1",	"apb1",
		      0x06c, BIT(1), 0);
static SUNXI_CCU_GATE(apb1_i2c2_clk,	"apb1-i2c2",	"apb1",
		      0x06c, BIT(2), 0);
static SUNXI_CCU_GATE(apb1_i2c3_clk,	"apb1-i2c3",	"apb1",
		      0x06c, BIT(3), 0);
static SUNXI_CCU_GATE(apb1_can_clk,	"apb1-can",	"apb1",
		      0x06c, BIT(4), 0);
static SUNXI_CCU_GATE(apb1_scr_clk,	"apb1-scr",	"apb1",
		      0x06c, BIT(5), 0);
static SUNXI_CCU_GATE(apb1_ps20_clk,	"apb1-ps20",	"apb1",
		      0x06c, BIT(6), 0);
static SUNXI_CCU_GATE(apb1_ps21_clk,	"apb1-ps21",	"apb1",
		      0x06c, BIT(7), 0);
/* BIT(8 .. 14) - reserved */
static SUNXI_CCU_GATE(apb1_i2c4_clk,	"apb1-i2c4",	"apb1",
		      0x06c, BIT(15), 0);
static SUNXI_CCU_GATE(apb1_uart0_clk,	"apb1-uart0",	"apb1",
		      0x06c, BIT(16), 0);
static SUNXI_CCU_GATE(apb1_uart1_clk,	"apb1-uart1",	"apb1",
		      0x06c, BIT(17), 0);
static SUNXI_CCU_GATE(apb1_uart2_clk,	"apb1-uart2",	"apb1",
		      0x06c, BIT(18), 0);
static SUNXI_CCU_GATE(apb1_uart3_clk,	"apb1-uart3",	"apb1",
		      0x06c, BIT(19), 0);
static SUNXI_CCU_GATE(apb1_uart4_clk,	"apb1-uart4",	"apb1",
		      0x06c, BIT(20), 0);
static SUNXI_CCU_GATE(apb1_uart5_clk,	"apb1-uart5",	"apb1",
		      0x06c, BIT(21), 0);
static SUNXI_CCU_GATE(apb1_uart6_clk,	"apb1-uart6",	"apb1",
		      0x06c, BIT(22), 0);
static SUNXI_CCU_GATE(apb1_uart7_clk,	"apb1-uart7",	"apb1",
		      0x06c, BIT(23), 0);
/* BIT(24 .. 31) - reserved */

static const char *const mod0_default_parents[] = { "hosc", "pll-periph",
						     "pll-ddr-other" };
static SUNXI_CCU_MP_WITH_MUX_GATE(nand_clk, "nand", mod0_default_parents, 0x080,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MP_WITH_MUX_GATE(ms_clk, "ms", mod0_default_parents, 0x084,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MP_WITH_MUX_GATE(mmc0_clk, "mmc0", mod0_default_parents, 0x088,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);
static SUNXI_CCU_PHASE(mmc0_output_clk, "mmc0_output", "mmc0",
		       0x088, 8, 3, 0);
static SUNXI_CCU_PHASE(mmc0_sample_clk, "mmc0_sample", "mmc0",
		       0x088, 20, 3, 0);

static SUNXI_CCU_MP_WITH_MUX_GATE(mmc1_clk, "mmc1", mod0_default_parents, 0x08c,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);
static SUNXI_CCU_PHASE(mmc1_output_clk, "mmc1_output", "mmc1",
		       0x08c, 8, 3, 0);
static SUNXI_CCU_PHASE(mmc1_sample_clk, "mmc1_sample", "mmc1",
		       0x08c, 20, 3, 0);

static SUNXI_CCU_MP_WITH_MUX_GATE(mmc2_clk, "mmc2", mod0_default_parents, 0x090,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);
static SUNXI_CCU_PHASE(mmc2_output_clk, "mmc2_output", "mmc2",
		       0x090, 8, 3, 0);
static SUNXI_CCU_PHASE(mmc2_sample_clk, "mmc2_sample", "mmc2",
		       0x090, 20, 3, 0);

static SUNXI_CCU_MP_WITH_MUX_GATE(mmc3_clk, "mmc3", mod0_default_parents, 0x094,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);
static SUNXI_CCU_PHASE(mmc3_output_clk, "mmc3_output", "mmc3",
		       0x094, 8, 3, 0);
static SUNXI_CCU_PHASE(mmc3_sample_clk, "mmc3_sample", "mmc3",
		       0x094, 20, 3, 0);

static SUNXI_CCU_MP_WITH_MUX_GATE(ts_clk, "ts", mod0_default_parents, 0x098,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MP_WITH_MUX_GATE(ss_clk, "ss", mod0_default_parents, 0x09c,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MP_WITH_MUX_GATE(spi0_clk, "spi0", mod0_default_parents, 0x0a0,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MP_WITH_MUX_GATE(spi1_clk, "spi1", mod0_default_parents, 0x0a4,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MP_WITH_MUX_GATE(spi2_clk, "spi2", mod0_default_parents, 0x0a8,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MP_WITH_MUX_GATE(pata_clk, "pata", mod0_default_parents, 0x0ac,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static const char *const ir_parents[] = { "hosc", "pll-periph",
					  "pll-ddr-other", "osc32k" };
static SUNXI_CCU_MP_WITH_MUX_GATE(ir0_clk, "ir0", ir_parents, 0x0b0,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MP_WITH_MUX_GATE(ir1_clk, "ir1", ir_parents, 0x0b4,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static const char *const audio_parents[] = { "pll-audio-8x", "pll-audio-4x",
					      "pll-audio-2x", "pll-audio" };
static SUNXI_CCU_MUX_WITH_GATE(i2s0_clk, "i2s0", audio_parents,
			       0x0b8, 16, 2, BIT(31), CLK_SET_RATE_PARENT);

static SUNXI_CCU_MUX_WITH_GATE(ac97_clk, "ac97", audio_parents,
			       0x0bc, 16, 2, BIT(31), CLK_SET_RATE_PARENT);

static SUNXI_CCU_MUX_WITH_GATE(spdif_clk, "spdif", audio_parents,
			       0x0c0, 16, 2, BIT(31), CLK_SET_RATE_PARENT);

/* TODO: keypad clock, 0x0c4 parents: 00: hosc, 10: osc32k */

/*
 * TODO: SATA clock also supports external clock as parent.
 * Currently we default to using PLL6 SATA gate.
 */
static SUNXI_CCU_GATE(sata_clk, "sata", "pll-periph-sata",
		      0x0c8, BIT(31), 0);

static SUNXI_CCU_GATE(usb_ohci0_clk,	"usb-ohci0",	"pll-periph",
		      0x0cc, BIT(6), 0);
static SUNXI_CCU_GATE(usb_ohci1_clk,	"usb-ohci1",	"pll-periph",
		      0x0cc, BIT(7), 0);
static SUNXI_CCU_GATE(usb_phy_clk,	"usb-phy",	"pll-periph",
		      0x0cc, BIT(8), 0);

static SUNXI_CCU_MP_WITH_MUX_GATE(spi3_clk, "spi3", mod0_default_parents, 0x0d4,
				  0, 4,		/* M */
				  16, 2,	/* P */
				  24, 2,	/* mux */
				  BIT(31),	/* gate */
				  0);

static SUNXI_CCU_MUX_WITH_GATE(i2s1_clk, "i2s1", audio_parents,
			       0x0d8, 16, 2, BIT(31), CLK_SET_RATE_PARENT);

static SUNXI_CCU_MUX_WITH_GATE(i2s2_clk, "i2s2", audio_parents,
			       0x0dc, 16, 2, BIT(31), CLK_SET_RATE_PARENT);

static SUNXI_CCU_GATE(dram_ve_clk,	"dram-ve",	"pll-ddr",
		      0x100, BIT(0), 0);
static SUNXI_CCU_GATE(dram_csi0_clk,	"dram-csi0",	"pll-ddr",
		      0x100, BIT(1), 0);
static SUNXI_CCU_GATE(dram_csi1_clk,	"dram-csi1",	"pll-ddr",
		      0x100, BIT(2), 0);
static SUNXI_CCU_GATE(dram_ts_clk,	"dram-ts",	"pll-ddr",
		      0x100, BIT(3), 0);
static SUNXI_CCU_GATE(dram_tvd_clk,	"dram-tvd",	"pll-ddr",
		      0x100, BIT(4), 0);
static SUNXI_CCU_GATE(dram_tve0_clk,	"dram-tve0",	"pll-ddr",
		      0x100, BIT(5), 0);
static SUNXI_CCU_GATE(dram_tve1_clk,	"dram-tve1",	"pll-ddr",
		      0x100, BIT(6), 0);
/* BIT(7 .. 14) - reserved */
static SUNXI_CCU_GATE(dram_out_clk,	"dram-out",	"pll-ddr",
		      0x100, BIT(15), 0);
/* BIT(16 .. 23) - reserved */
static SUNXI_CCU_GATE(dram_de_fe1_clk,	"dram-de-fe1",	"pll-ddr",
		      0x100, BIT(24), 0);
static SUNXI_CCU_GATE(dram_de_fe0_clk,	"dram-de-fe0",	"pll-ddr",
		      0x100, BIT(25), 0);
static SUNXI_CCU_GATE(dram_de_be0_clk,	"dram-de-be0",	"pll-ddr",
		      0x100, BIT(26), 0);
static SUNXI_CCU_GATE(dram_de_be1_clk,	"dram-de-be1",	"pll-ddr",
		      0x100, BIT(27), 0);
static SUNXI_CCU_GATE(dram_mp_clk,	"dram-mp",	"pll-ddr",
		      0x100, BIT(28), 0);
static SUNXI_CCU_GATE(dram_ace_clk,	"dram-ace",	"pll-ddr",
		      0x100, BIT(29), 0);
/* BIT(30 .. 31) - reserved */

static const char *const de_parents[] = { "pll-video0", "pll-video1",
					   "pll-ddr-other" };
static SUNXI_CCU_M_WITH_MUX_GATE(de_be0_clk, "de-be0", de_parents,
				 0x104, 0, 4, 24, 2, BIT(31), 0);

static SUNXI_CCU_M_WITH_MUX_GATE(de_be1_clk, "de-be1", de_parents,
				 0x108, 0, 4, 24, 2, BIT(31), 0);

static SUNXI_CCU_M_WITH_MUX_GATE(de_fe0_clk, "de-fe0", de_parents,
				 0x10c, 0, 4, 24, 2, BIT(31), 0);

static SUNXI_CCU_M_WITH_MUX_GATE(de_fe1_clk, "de-fe1", de_parents,
				 0x110, 0, 4, 24, 2, BIT(31), 0);

static SUNXI_CCU_M_WITH_MUX_GATE(de_mp_clk, "de-mp", de_parents,
				 0x114, 0, 4, 24, 2, BIT(31), 0);

static const char *const tcon_parents[] = { "pll-video0", "pll-video1",
					    "pll-video0-2x", "pll-video1-2x" };
static SUNXI_CCU_MUX_WITH_GATE(tcon0_ch0_clk, "tcon0-ch0-sclk", tcon_parents,
			       0x118, 24, 2, BIT(31), CLK_SET_RATE_PARENT);
static SUNXI_CCU_MUX_WITH_GATE(tcon1_ch0_clk, "tcon1-ch0-sclk", tcon_parents,
			       0x11c, 24, 2, BIT(31), CLK_SET_RATE_PARENT);

/* TODO: CSI special clock register - 0x120 */
/* TODO: TVD clock register - 0x128 */

static SUNXI_CCU_M_WITH_MUX_GATE(tcon0_ch1_sclk2_clk, "tcon0-ch1-sclk2",
				 tcon_parents,
				 0x12c, 0, 4, 24, 2, BIT(31),
				 CLK_SET_RATE_PARENT);

static SUNXI_CCU_M_WITH_GATE(tcon0_ch1_clk,
			     "tcon0-ch1-sclk1", "tcon0-ch1-sclk2",
			     0x12c, 11, 1, BIT(15),
			     CLK_SET_RATE_PARENT);

static SUNXI_CCU_M_WITH_MUX_GATE(tcon1_ch1_sclk2_clk, "tcon1-ch1-sclk2",
				 tcon_parents,
				 0x130, 0, 4, 24, 2, BIT(31),
				 CLK_SET_RATE_PARENT);

static SUNXI_CCU_M_WITH_GATE(tcon1_ch1_clk,
			     "tcon1-ch1-sclk1", "tcon1-ch1-sclk2",
			     0x130, 11, 1, BIT(15),
			     CLK_SET_RATE_PARENT);

/* TODO: CSI0 clock - 0x134 */
/* TODO: CSI1 clock - 0x138 */
static SUNXI_CCU_M_WITH_GATE(ve_clk, "ve", "pll-ve",
			     0x13c, 16, 8, BIT(31), 0);

static SUNXI_CCU_GATE(codec_clk,	"codec",	"pll-audio",
		      0x140, BIT(31), CLK_SET_RATE_PARENT);
static SUNXI_CCU_GATE(avs_clk,		"avs",		"hosc",
		      0x144, BIT(31), 0);
/* TODO: ACE clock - 0x148 */
/* TODO: HDMI clock - 0x150 */
/* TODO: GPU clock - 0x154 */

static const char *const mbus_parents[] = { "hosc", "pll-periph-2x",
					    "pll-ddr-other" };
static SUNXI_CCU_MP_WITH_MUX_GATE(mbus_clk, "mbus", mbus_parents,
				  0x15c, 0, 4, 16, 2, 24, 2, BIT(31),
				  CLK_IS_CRITICAL);

/* TODO: HDMI1 slow clock 0x178 */
/* TODO: REPEAT clock 0x17c */
static const char *const out_parents[] = { "hosc", "osc32k", "hosc" };
static SUNXI_CCU_MP_WITH_MUX_GATE(out_a_clk, "out-a", out_parents,
				  0x1f0, 8, 5, 20, 2, 24, 2, BIT(31), 0);
static SUNXI_CCU_MP_WITH_MUX_GATE(out_b_clk, "out-b", out_parents,
				  0x1f4, 8, 5, 20, 2, 24, 2, BIT(31), 0);

static struct ccu_common *sun7i_a20_ccu_clks[] = {
	&hosc_clk.common,
	&pll_core_clk.common,
	&pll_audio_base_clk.common,
	&pll_video0_clk.common,
	&pll_ve_clk.common,
	&pll_ddr_base_clk.common,
	&pll_ddr_clk.common,
	&pll_ddr_other_clk.common,
	&pll_periph_clk.common,
	&pll_periph_sata_clk.common,
	&pll_video1_clk.common,
	&cpu_clk.common,
	&axi_clk.common,
	&ahb_clk.common,
	&apb0_clk.common,
	&apb1_clk.common,
	&ahb_otg_clk.common,
	&ahb_ehci0_clk.common,
	&ahb_ohci0_clk.common,
	&ahb_ehci1_clk.common,
	&ahb_ohci1_clk.common,
	&ahb_ss_clk.common,
	&ahb_dma_clk.common,
	&ahb_bist_clk.common,
	&ahb_mmc0_clk.common,
	&ahb_mmc1_clk.common,
	&ahb_mmc2_clk.common,
	&ahb_mmc3_clk.common,
	&ahb_ms_clk.common,
	&ahb_nand_clk.common,
	&ahb_sdram_clk.common,
	&ahb_ace_clk.common,
	&ahb_emac_clk.common,
	&ahb_ts_clk.common,
	&ahb_spi0_clk.common,
	&ahb_spi1_clk.common,
	&ahb_spi2_clk.common,
	&ahb_spi3_clk.common,
	&ahb_sata_clk.common,
	&ahb_hstimer_clk.common,
	&ahb_ve_clk.common,
	&ahb_tvd_clk.common,
	&ahb_tve0_clk.common,
	&ahb_tve1_clk.common,
	&ahb_lcd0_clk.common,
	&ahb_lcd1_clk.common,
	&ahb_csi0_clk.common,
	&ahb_csi1_clk.common,
	&ahb_hdmi1_clk.common,
	&ahb_hdmi0_clk.common,
	&ahb_de_be0_clk.common,
	&ahb_de_be1_clk.common,
	&ahb_de_fe0_clk.common,
	&ahb_de_fe1_clk.common,
	&ahb_gmac_clk.common,
	&ahb_mp_clk.common,
	&ahb_gpu_clk.common,
	&apb0_codec_clk.common,
	&apb0_spdif_clk.common,
	&apb0_ac97_clk.common,
	&apb0_i2s0_clk.common,
	&apb0_i2s1_clk.common,
	&apb0_pio_clk.common,
	&apb0_ir0_clk.common,
	&apb0_ir1_clk.common,
	&apb0_i2s2_clk.common,
	&apb0_keypad_clk.common,
	&apb1_i2c0_clk.common,
	&apb1_i2c1_clk.common,
	&apb1_i2c2_clk.common,
	&apb1_i2c3_clk.common,
	&apb1_can_clk.common,
	&apb1_scr_clk.common,
	&apb1_ps20_clk.common,
	&apb1_ps21_clk.common,
	&apb1_i2c4_clk.common,
	&apb1_uart0_clk.common,
	&apb1_uart1_clk.common,
	&apb1_uart2_clk.common,
	&apb1_uart3_clk.common,
	&apb1_uart4_clk.common,
	&apb1_uart5_clk.common,
	&apb1_uart6_clk.common,
	&apb1_uart7_clk.common,
	&nand_clk.common,
	&ms_clk.common,
	&mmc0_clk.common,
	&mmc0_output_clk.common,
	&mmc0_sample_clk.common,
	&mmc1_clk.common,
	&mmc1_output_clk.common,
	&mmc1_sample_clk.common,
	&mmc2_clk.common,
	&mmc2_output_clk.common,
	&mmc2_sample_clk.common,
	&mmc3_clk.common,
	&mmc3_output_clk.common,
	&mmc3_sample_clk.common,
	&ts_clk.common,
	&ss_clk.common,
	&spi0_clk.common,
	&spi1_clk.common,
	&spi2_clk.common,
	&pata_clk.common,
	&ir0_clk.common,
	&ir1_clk.common,
	&i2s0_clk.common,
	&ac97_clk.common,
	&spdif_clk.common,
//	&keypad_clk.common,
	&sata_clk.common,
	&usb_ohci0_clk.common,
	&usb_ohci1_clk.common,
	&usb_phy_clk.common,
	&spi3_clk.common,
	&i2s1_clk.common,
	&i2s2_clk.common,
	&dram_ve_clk.common,
	&dram_csi0_clk.common,
	&dram_csi1_clk.common,
	&dram_ts_clk.common,
	&dram_tvd_clk.common,
	&dram_tve0_clk.common,
	&dram_tve1_clk.common,
	&dram_out_clk.common,
	&dram_de_fe1_clk.common,
	&dram_de_fe0_clk.common,
	&dram_de_be0_clk.common,
	&dram_de_be1_clk.common,
	&dram_mp_clk.common,
	&dram_ace_clk.common,
	&de_be0_clk.common,
	&de_be1_clk.common,
	&de_fe0_clk.common,
	&de_fe1_clk.common,
	&de_mp_clk.common,
	&tcon0_ch0_clk.common,
	&tcon1_ch0_clk.common,
//	&csi_special_clk.common,
//	&tvd_clk.common,
	&tcon0_ch1_sclk2_clk.common,
	&tcon0_ch1_clk.common,
	&tcon1_ch1_sclk2_clk.common,
	&tcon1_ch1_clk.common,
//	&csi0_clk.common,
//	&csi1_clk.common,
	&ve_clk.common,
	&codec_clk.common,
	&avs_clk.common,
//	&ace_clk.common,
//	&hdmi_clk.common,
//	&gpu_clk.common,
	&mbus_clk.common,
//	&hdmi1_slow_clk.common,
//	&hdmi1_repeat_clk.common,
	&out_a_clk.common,
	&out_b_clk.common
};

static CLK_FIXED_FACTOR(pll_periph_2x_clk, "pll-periph-2x",
			"pll-periph", 1, 2, CLK_SET_RATE_PARENT);
/* We hardcode the divider to 4 for now */
static CLK_FIXED_FACTOR(pll_audio_clk, "pll-audio",
			"pll-audio-base", 4, 1, CLK_SET_RATE_PARENT);
static CLK_FIXED_FACTOR(pll_audio_2x_clk, "pll-audio-2x",
			"pll-audio-base", 2, 1, CLK_SET_RATE_PARENT);
static CLK_FIXED_FACTOR(pll_audio_4x_clk, "pll-audio-4x",
			"pll-audio-base", 1, 1, CLK_SET_RATE_PARENT);
static CLK_FIXED_FACTOR(pll_audio_8x_clk, "pll-audio-8x",
			"pll-audio-base", 1, 2, CLK_SET_RATE_PARENT);
static CLK_FIXED_FACTOR(pll_video0_2x_clk, "pll-video0-2x",
			"pll-video0", 1, 2, CLK_SET_RATE_PARENT);
static CLK_FIXED_FACTOR(pll_video1_2x_clk, "pll-video1-2x",
			"pll-video1", 1, 2, CLK_SET_RATE_PARENT);


static struct clk_hw_onecell_data sun7i_a20_hw_clks = {
	.hws	= {
		[CLK_HOSC]		= &hosc_clk.common.hw,
		[CLK_PLL_CORE]		= &pll_core_clk.common.hw,
		[CLK_PLL_AUDIO_BASE]	= &pll_audio_base_clk.common.hw,
		[CLK_PLL_AUDIO]		= &pll_audio_clk.hw,
		[CLK_PLL_AUDIO_2X]	= &pll_audio_2x_clk.hw,
		[CLK_PLL_AUDIO_4X]	= &pll_audio_4x_clk.hw,
		[CLK_PLL_AUDIO_8X]	= &pll_audio_8x_clk.hw,
		[CLK_PLL_VIDEO0]	= &pll_video0_clk.common.hw,
		[CLK_PLL_VIDEO0_2X]	= &pll_video0_2x_clk.hw,
		[CLK_PLL_VE]		= &pll_ve_clk.common.hw,
		[CLK_PLL_DDR_BASE]	= &pll_ddr_base_clk.common.hw,
		[CLK_PLL_DDR]		= &pll_ddr_clk.common.hw,
		[CLK_PLL_DDR_OTHER]	= &pll_ddr_other_clk.common.hw,
		[CLK_PLL_PERIPH]	= &pll_periph_clk.common.hw,
		[CLK_PLL_PERIPH_2X]	= &pll_periph_2x_clk.hw,
		[CLK_PLL_PERIPH_SATA]	= &pll_periph_sata_clk.common.hw,
		[CLK_PLL_VIDEO1]	= &pll_video1_clk.common.hw,
		[CLK_PLL_VIDEO1_2X]	= &pll_video1_2x_clk.hw,
		[CLK_CPU]		= &cpu_clk.common.hw,
		[CLK_AXI]		= &axi_clk.common.hw,
		[CLK_AHB]		= &ahb_clk.common.hw,
		[CLK_APB0]		= &apb0_clk.common.hw,
		[CLK_APB1]		= &apb1_clk.common.hw,
		[CLK_AHB_OTG]		= &ahb_otg_clk.common.hw,
		[CLK_AHB_EHCI0]		= &ahb_ehci0_clk.common.hw,
		[CLK_AHB_OHCI0]		= &ahb_ohci0_clk.common.hw,
		[CLK_AHB_EHCI1]		= &ahb_ehci1_clk.common.hw,
		[CLK_AHB_OHCI1]		= &ahb_ohci1_clk.common.hw,
		[CLK_AHB_SS]		= &ahb_ss_clk.common.hw,
		[CLK_AHB_DMA]		= &ahb_dma_clk.common.hw,
		[CLK_AHB_BIST]		= &ahb_bist_clk.common.hw,
		[CLK_AHB_MMC0]		= &ahb_mmc0_clk.common.hw,
		[CLK_AHB_MMC1]		= &ahb_mmc1_clk.common.hw,
		[CLK_AHB_MMC2]		= &ahb_mmc2_clk.common.hw,
		[CLK_AHB_MMC3]		= &ahb_mmc3_clk.common.hw,
		[CLK_AHB_MS]		= &ahb_ms_clk.common.hw,
		[CLK_AHB_NAND]		= &ahb_nand_clk.common.hw,
		[CLK_AHB_SDRAM]		= &ahb_sdram_clk.common.hw,
		[CLK_AHB_ACE]		= &ahb_ace_clk.common.hw,
		[CLK_AHB_EMAC]		= &ahb_emac_clk.common.hw,
		[CLK_AHB_TS]		= &ahb_ts_clk.common.hw,
		[CLK_AHB_SPI0]		= &ahb_spi0_clk.common.hw,
		[CLK_AHB_SPI1]		= &ahb_spi1_clk.common.hw,
		[CLK_AHB_SPI2]		= &ahb_spi2_clk.common.hw,
		[CLK_AHB_SPI3]		= &ahb_spi3_clk.common.hw,
		[CLK_AHB_SATA]		= &ahb_sata_clk.common.hw,
		[CLK_AHB_HSTIMER]	= &ahb_hstimer_clk.common.hw,
		[CLK_AHB_VE]		= &ahb_ve_clk.common.hw,
		[CLK_AHB_TVD]		= &ahb_tvd_clk.common.hw,
		[CLK_AHB_TVE0]		= &ahb_tve0_clk.common.hw,
		[CLK_AHB_TVE1]		= &ahb_tve1_clk.common.hw,
		[CLK_AHB_LCD0]		= &ahb_lcd0_clk.common.hw,
		[CLK_AHB_LCD1]		= &ahb_lcd1_clk.common.hw,
		[CLK_AHB_CSI0]		= &ahb_csi0_clk.common.hw,
		[CLK_AHB_CSI1]		= &ahb_csi1_clk.common.hw,
		[CLK_AHB_HDMI1]		= &ahb_hdmi1_clk.common.hw,
		[CLK_AHB_HDMI0]		= &ahb_hdmi0_clk.common.hw,
		[CLK_AHB_DE_BE0]	= &ahb_de_be0_clk.common.hw,
		[CLK_AHB_DE_BE1]	= &ahb_de_be1_clk.common.hw,
		[CLK_AHB_DE_FE0]	= &ahb_de_fe0_clk.common.hw,
		[CLK_AHB_DE_FE1]	= &ahb_de_fe1_clk.common.hw,
		[CLK_AHB_GMAC]		= &ahb_gmac_clk.common.hw,
		[CLK_AHB_MP]		= &ahb_mp_clk.common.hw,
		[CLK_AHB_GPU]		= &ahb_gpu_clk.common.hw,
		[CLK_APB0_CODEC]	= &apb0_codec_clk.common.hw,
		[CLK_APB0_SPDIF]	= &apb0_spdif_clk.common.hw,
		[CLK_APB0_AC97]		= &apb0_ac97_clk.common.hw,
		[CLK_APB0_I2S0]		= &apb0_i2s0_clk.common.hw,
		[CLK_APB0_I2S1]		= &apb0_i2s1_clk.common.hw,
		[CLK_APB0_PIO]		= &apb0_pio_clk.common.hw,
		[CLK_APB0_IR0]		= &apb0_ir0_clk.common.hw,
		[CLK_APB0_IR1]		= &apb0_ir1_clk.common.hw,
		[CLK_APB0_I2S2]		= &apb0_i2s2_clk.common.hw,
		[CLK_APB0_KEYPAD]	= &apb0_keypad_clk.common.hw,
		[CLK_APB1_I2C0]		= &apb1_i2c0_clk.common.hw,
		[CLK_APB1_I2C1]		= &apb1_i2c1_clk.common.hw,
		[CLK_APB1_I2C2]		= &apb1_i2c2_clk.common.hw,
		[CLK_APB1_I2C3]		= &apb1_i2c3_clk.common.hw,
		[CLK_APB1_CAN]		= &apb1_can_clk.common.hw,
		[CLK_APB1_SCR]		= &apb1_scr_clk.common.hw,
		[CLK_APB1_PS20]		= &apb1_ps20_clk.common.hw,
		[CLK_APB1_PS21]		= &apb1_ps21_clk.common.hw,
		[CLK_APB1_I2C4]		= &apb1_i2c4_clk.common.hw,
		[CLK_APB1_UART0]	= &apb1_uart0_clk.common.hw,
		[CLK_APB1_UART1]	= &apb1_uart1_clk.common.hw,
		[CLK_APB1_UART2]	= &apb1_uart2_clk.common.hw,
		[CLK_APB1_UART3]	= &apb1_uart3_clk.common.hw,
		[CLK_APB1_UART4]	= &apb1_uart4_clk.common.hw,
		[CLK_APB1_UART5]	= &apb1_uart5_clk.common.hw,
		[CLK_APB1_UART6]	= &apb1_uart6_clk.common.hw,
		[CLK_APB1_UART7]	= &apb1_uart7_clk.common.hw,
		[CLK_NAND]		= &nand_clk.common.hw,
		[CLK_MS]		= &ms_clk.common.hw,
		[CLK_MMC0]		= &mmc0_clk.common.hw,
		[CLK_MMC0_OUTPUT]	= &mmc0_output_clk.common.hw,
		[CLK_MMC0_SAMPLE]	= &mmc0_sample_clk.common.hw,
		[CLK_MMC1]		= &mmc1_clk.common.hw,
		[CLK_MMC1_OUTPUT]	= &mmc1_output_clk.common.hw,
		[CLK_MMC1_SAMPLE]	= &mmc1_sample_clk.common.hw,
		[CLK_MMC2]		= &mmc2_clk.common.hw,
		[CLK_MMC2_OUTPUT]	= &mmc2_output_clk.common.hw,
		[CLK_MMC2_SAMPLE]	= &mmc2_sample_clk.common.hw,
		[CLK_MMC3]		= &mmc3_clk.common.hw,
		[CLK_MMC3_OUTPUT]	= &mmc3_output_clk.common.hw,
		[CLK_MMC3_SAMPLE]	= &mmc3_sample_clk.common.hw,
		[CLK_TS]		= &ts_clk.common.hw,
		[CLK_SS]		= &ss_clk.common.hw,
		[CLK_SPI0]		= &spi0_clk.common.hw,
		[CLK_SPI1]		= &spi1_clk.common.hw,
		[CLK_SPI2]		= &spi2_clk.common.hw,
		[CLK_PATA]		= &pata_clk.common.hw,
		[CLK_IR0]		= &ir0_clk.common.hw,
		[CLK_IR1]		= &ir1_clk.common.hw,
		[CLK_I2S0]		= &i2s0_clk.common.hw,
		[CLK_AC97]		= &ac97_clk.common.hw,
		[CLK_SPDIF]		= &spdif_clk.common.hw,
//		[CLK_KEYPAD]		= &keypad_clk.common.hw,
		[CLK_SATA]		= &sata_clk.common.hw,
		[CLK_USB_OHCI0]		= &usb_ohci0_clk.common.hw,
		[CLK_USB_OHCI1]		= &usb_ohci1_clk.common.hw,
		[CLK_USB_PHY]		= &usb_phy_clk.common.hw,
		[CLK_SPI3]		= &spi3_clk.common.hw,
		[CLK_I2S1]		= &i2s1_clk.common.hw,
		[CLK_I2S2]		= &i2s2_clk.common.hw,
		[CLK_DRAM_VE]		= &dram_ve_clk.common.hw,
		[CLK_DRAM_CSI0]		= &dram_csi0_clk.common.hw,
		[CLK_DRAM_CSI1]		= &dram_csi1_clk.common.hw,
		[CLK_DRAM_TS]		= &dram_ts_clk.common.hw,
		[CLK_DRAM_TVD]		= &dram_tvd_clk.common.hw,
		[CLK_DRAM_TVE0]		= &dram_tve0_clk.common.hw,
		[CLK_DRAM_TVE1]		= &dram_tve1_clk.common.hw,
		[CLK_DRAM_OUT]		= &dram_out_clk.common.hw,
		[CLK_DRAM_DE_FE1]	= &dram_de_fe1_clk.common.hw,
		[CLK_DRAM_DE_FE0]	= &dram_de_fe0_clk.common.hw,
		[CLK_DRAM_DE_BE0]	= &dram_de_be0_clk.common.hw,
		[CLK_DRAM_DE_BE1]	= &dram_de_be1_clk.common.hw,
		[CLK_DRAM_MP]		= &dram_mp_clk.common.hw,
		[CLK_DRAM_ACE]		= &dram_ace_clk.common.hw,
		[CLK_DE_BE0]		= &de_be0_clk.common.hw,
		[CLK_DE_BE1]		= &de_be1_clk.common.hw,
		[CLK_DE_FE0]		= &de_fe0_clk.common.hw,
		[CLK_DE_FE1]		= &de_fe1_clk.common.hw,
		[CLK_DE_MP]		= &de_mp_clk.common.hw,
		[CLK_TCON0_CH0]		= &tcon0_ch0_clk.common.hw,
		[CLK_TCON1_CH0]		= &tcon1_ch0_clk.common.hw,
//		[CLK_CSI_SPECIAL]	= &csi_special_clk.common.hw,
//		[CLK_TVD]		= &tvd_clk.common.hw,
		[CLK_TCON0_CH1_SCLK2]	= &tcon0_ch1_sclk2_clk.common.hw,
		[CLK_TCON0_CH1]		= &tcon0_ch1_clk.common.hw,
		[CLK_TCON1_CH1_SCLK2]	= &tcon1_ch1_sclk2_clk.common.hw,
		[CLK_TCON1_CH1]		= &tcon1_ch1_clk.common.hw,
//		[CLK_CSI0]		= &csi0_clk.common.hw,
//		[CLK_CSI1]		= &csi1_clk.common.hw,
		[CLK_VE]		= &ve_clk.common.hw,
		[CLK_CODEC]		= &codec_clk.common.hw,
		[CLK_AVS]		= &avs_clk.common.hw,
//		[CLK_ACE]		= &ace_clk.common.hw,
//		[CLK_HDMI]		= &hdmi_clk.common.hw,
//		[CLK_GPU]		= &gpu_clk.common.hw,
		[CLK_MBUS]		= &mbus_clk.common.hw,
//		[CLK_HDMI1_SLOW]	= &hdmi1_slow_clk.common.hw,
//		[CLK_HDMI1_REPEAT]	= &hdmi1_repeat_clk.common.hw,
		[CLK_OUT_A]		= &out_a_clk.common.hw,
		[CLK_OUT_B]		= &out_b_clk.common.hw,
	},
	.num	= CLK_NUMBER,
};

static struct ccu_reset_map sun7i_a20_ccu_resets[] = {

	[RST_USB_PHY0]		= { 0x0cc, BIT(0) },
	[RST_USB_PHY1]		= { 0x0cc, BIT(1) },
	[RST_USB_PHY2]		= { 0x0cc, BIT(2) },
	[RST_DE_BE0]		= { 0x104, BIT(30) },
	[RST_DE_BE1]		= { 0x108, BIT(30) },
	[RST_DE_FE0]		= { 0x10c, BIT(30) },
	[RST_DE_FE1]		= { 0x110, BIT(30) },
	[RST_DE_MP]		= { 0x114, BIT(30) },
	[RST_TCON0]		= { 0x118, BIT(30) },
	[RST_TCON1]		= { 0x11c, BIT(30) },
	[RST_CSI0]		= { 0x134, BIT(30) },
	[RST_CSI1]		= { 0x138, BIT(30) },
	[RST_VE]		= { 0x13c, BIT(0) },
	[RST_ACE]		= { 0x148, BIT(16) },
	[RST_LVDS]		= { 0x14c, BIT(0) },
	[RST_GPU]		= { 0x154, BIT(30) },
	[RST_HDMI_H]		= { 0x170, BIT(0) },
	[RST_HDMI_SYS]		= { 0x170, BIT(1) },
	[RST_HDMI_AUDIO_DMA]	= { 0x170, BIT(2) },
};

static const struct sunxi_ccu_desc sun7i_a20_ccu_desc = {
	.ccu_clks	= sun7i_a20_ccu_clks,
	.num_ccu_clks	= ARRAY_SIZE(sun7i_a20_ccu_clks),

	.hw_clks	= &sun7i_a20_hw_clks,

	.resets		= sun7i_a20_ccu_resets,
	.num_resets	= ARRAY_SIZE(sun7i_a20_ccu_resets),
};

static void __init sun7i_a20_ccu_setup(struct device_node *node)
{
	void __iomem *reg;
	u32 val;

	reg = of_io_request_and_map(node, 0, of_node_full_name(node));
	if (IS_ERR(reg)) {
		pr_err("%s: Could not map the clock registers\n",
		       of_node_full_name(node));
		return;
	}

	#define SUN7I_PLL_AUDIO_REG	0x008

	/* Force the PLL-Audio-1x divider to 4 */
	val = readl(reg + SUN7I_PLL_AUDIO_REG);
	val &= ~GENMASK(19, 16);
	writel(val | (3 << 16), reg + SUN7I_PLL_AUDIO_REG);

	/*
	 * Use PLL6 as parent for AHB
	 * CPU/AXI clock changes rate when cpufreq is enabled
	 */
	#define SUN7I_AHB_REG		0x054
	val = readl(reg + SUN7I_AHB_REG);
	val &= ~GENMASK(7, 6);
	writel(val | (2 << 6), reg + SUN7I_AHB_REG);

	sunxi_ccu_probe(node, reg, &sun7i_a20_ccu_desc);
}
CLK_OF_DECLARE(sun7i_a20_ccu, "allwinner,sun7i-a20-ccu",
	       sun7i_a20_ccu_setup);
