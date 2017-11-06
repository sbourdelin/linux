/*
 * Copyright (c) 2014 Actions Semi Inc.
 * Author: David Liu <liuwei@actions-semi.com>
 *
 * Copyright (c) 2017 Linaro Ltd.
 * Author: Manivannan Sadhasivam <manivannan.sadhasivam@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include <dt-bindings/clock/actions,s900-cmu.h>
#include "owl-clk.h"

#define CMU_COREPLL		(0x0000)
#define CMU_DEVPLL		(0x0004)
#define CMU_DDRPLL		(0x0008)
#define CMU_NANDPLL		(0x000C)
#define CMU_DISPLAYPLL		(0x0010)
#define CMU_AUDIOPLL		(0x0014)
#define CMU_TVOUTPLL		(0x0018)
#define CMU_BUSCLK		(0x001C)
#define CMU_SENSORCLK		(0x0020)
#define CMU_LCDCLK		(0x0024)
#define CMU_DSICLK		(0x0028)
#define CMU_CSICLK		(0x002C)
#define CMU_DECLK		(0x0030)
#define CMU_BISPCLK		(0x0034)
#define CMU_IMXCLK		(0x0038)
#define CMU_HDECLK		(0x003C)
#define CMU_VDECLK		(0x0040)
#define CMU_VCECLK		(0x0044)
#define CMU_NANDCCLK		(0x004C)
#define CMU_SD0CLK		(0x0050)
#define CMU_SD1CLK		(0x0054)
#define CMU_SD2CLK		(0x0058)
#define CMU_UART0CLK		(0x005C)
#define CMU_UART1CLK		(0x0060)
#define CMU_UART2CLK		(0x0064)
#define CMU_PWM0CLK		(0x0070)
#define CMU_PWM1CLK		(0x0074)
#define CMU_PWM2CLK		(0x0078)
#define CMU_PWM3CLK		(0x007C)
#define CMU_USBPLL		(0x0080)
#define CMU_ASSISTPLL		(0x0084)
#define CMU_EDPCLK		(0x0088)
#define CMU_GPU3DCLK		(0x0090)
#define CMU_CORECTL		(0x009C)
#define CMU_DEVCLKEN0		(0x00A0)
#define CMU_DEVCLKEN1		(0x00A4)
#define CMU_DEVRST0		(0x00A8)
#define CMU_DEVRST1		(0x00AC)
#define CMU_UART3CLK		(0x00B0)
#define CMU_UART4CLK		(0x00B4)
#define CMU_UART5CLK		(0x00B8)
#define CMU_UART6CLK		(0x00BC)
#define CMU_TLSCLK		(0x00C0)
#define CMU_SD3CLK		(0x00C4)
#define CMU_PWM4CLK		(0x00C8)
#define CMU_PWM5CLK		(0x00CC)

static struct clk_pll_table clk_audio_pll_table[] = {
	{0, 45158400}, {1, 49152000},
	{0, 0},
};

static struct clk_pll_table clk_edp_pll_table[] = {
	{0, 810000000}, {1, 1350000000}, {2, 2700000000},
	{0, 0},
};

/* pll clocks */
static struct owl_pll_clock s900_pll_clks[] = {
	{ CLK_CORE_PLL, "core_pll", NULL, CLK_IGNORE_UNUSED, CMU_COREPLL, 24000000, 9, 0, 8,  5, 107, 0, NULL, },
	{ CLK_DEV_PLL, "dev_pll",  NULL, CLK_IGNORE_UNUSED, CMU_DEVPLL, 6000000, 8, 0, 8, 20, 180, 0, NULL, },
	{ CLK_DDR_PLL, "ddr_pll",  NULL, CLK_IGNORE_UNUSED, CMU_DDRPLL, 24000000, 8, 0, 8,  5,  45, 0, NULL, },
	{ CLK_NAND_PLL, "nand_pll", NULL, CLK_IGNORE_UNUSED, CMU_NANDPLL, 6000000, 8, 0, 8,  4, 100, 0, NULL, },
	{ CLK_DISPLAY_PLL, "display_pll", NULL, CLK_IGNORE_UNUSED, 0, 6000000, 8, 0, 8, 20, 180, 0, NULL, },
	{ CLK_ASSIST_PLL, "assist_pll", NULL, CLK_IGNORE_UNUSED, 0, 500000000, 0, 0, 0, 0, 0, CLK_OWL_PLL_FIXED_FREQ, NULL, },
	{ CLK_AUDIO_PLL, "audio_pll", NULL, CLK_IGNORE_UNUSED, 0, 0, 4, 0, 1, 0, 0, 0, clk_audio_pll_table, },

	{ CLK_EDP_PLL, "edp_pll", "24M_edp", CLK_IGNORE_UNUSED, 0, 0, 9, 0, 2, 0, 0, 0, clk_edp_pll_table, },
};

static const char *cpu_clk_mux_p[] = { "losc", "hosc", "core_pll", };
static const char *dev_clk_p[] = { "hosc", "dev_pll", };
static const char *noc_clk_mux_p[] = { "dev_clk", "assist_pll", };
static const char *dmm_clk_mux_p[] = { "dev_clk", "nand_pll", "assist_pll", "ddr_clk_src", };

static const char *bisp_clk_mux_p[] = { "assist_pll", "dev_clk", };
static const char *csi_clk_mux_p[] = { "display_pll", "dev_clk", };
static const char *de_clk_mux_p[] = { "assist_pll", "dev_clk", };
static const char *eth_mac_clk_mux_p[] = { "assist_pll", };
static const char *gpu_clk_mux_p[] = { "dev_clk", "display_pll", "", "ddr_clk_src", };
static const char *hde_clk_mux_p[] = { "dev_clk", "display_pll", "", "ddr_clk_src", };
static const char *i2c_clk_mux_p[] = { "assist_pll", };
static const char *imx_clk_mux_p[] = { "assist_pll", "dev_clk", };
static const char *lcd_clk_mux_p[] = { "display_pll", "nand_pll", };
static const char *nand_clk_mux_p[] = { "dev_clk", "nand_pll", };
static const char *pwm_clk_mux_p[] = { "hosc" };
static const char *sd_clk_mux_p[] = { "dev_clk", "nand_pll", };
static const char *sensor_clk_mux_p[] = { "hosc", "bisp", };
static const char *speed_sensor_clk_mux_p[] = { "hosc", };
static const char *spi_clk_mux_p[] = { "ahb_clk", };
static const char *thermal_sensor_clk_mux_p[] = { "hosc", };
static const char *uart_clk_mux_p[] = { "hosc", "dev_pll", };
static const char *vce_clk_mux_p[] = { "dev_clk", "display_pll", "assist_pll", "ddr_clk_src", };
static const char *i2s_clk_mux_p[] = { "audio_pll", };

static const char *edp_clk_mux_p[] = { "assist_pll", "display_pll", };

/* mux clocks */
static struct owl_mux_clock s900_mux_clks[] = {
	{ CLK_CPU,  "cpu_clk", cpu_clk_mux_p, ARRAY_SIZE(cpu_clk_mux_p), CLK_SET_RATE_PARENT, CMU_BUSCLK, 0, 2, 0, "cpu_clk", },
	{ CLK_DEV,  "dev_clk", dev_clk_p, ARRAY_SIZE(dev_clk_p), CLK_SET_RATE_PARENT, CMU_DEVPLL, 12, 1, 0, "dev_clk", },
	{ CLK_NOC_CLK_MUX,  "noc_clk_mux", noc_clk_mux_p, ARRAY_SIZE(noc_clk_mux_p), CLK_SET_RATE_PARENT, CMU_BUSCLK, 7, 1, 0, },
};

static struct clk_div_table nand_div_table[] = {
	{0, 1},   {1, 2},   {2, 4},   {3, 6},
	{4, 8},   {5, 10},  {6, 12},  {7, 14},
	{8, 16},  {9, 18},  {10, 20}, {11, 22},
	{12, 24}, {13, 26}, {14, 28}, {15, 30},
	{0, 0},
};

static struct clk_factor_table sd_factor_table[] = {
	/* bit0 ~ 4 */
	{0, 1, 1}, {1, 1, 2}, {2, 1, 3}, {3, 1, 4},
	{4, 1, 5}, {5, 1, 6}, {6, 1, 7}, {7, 1, 8},
	{8, 1, 9}, {9, 1, 10}, {10, 1, 11}, {11, 1, 12},
	{12, 1, 13}, {13, 1, 14}, {14, 1, 15}, {15, 1, 16},
	{16, 1, 17}, {17, 1, 18}, {18, 1, 19}, {19, 1, 20},
	{20, 1, 21}, {21, 1, 22}, {22, 1, 23}, {23, 1, 24},
	{24, 1, 25}, {25, 1, 26}, {26, 1, 27}, {27, 1, 28},
	{28, 1, 29}, {29, 1, 30}, {30, 1, 31}, {31, 1, 32},

	/* bit8: /128 */
	{256, 1, 1 * 128}, {257, 1, 2 * 128}, {258, 1, 3 * 128}, {259, 1, 4 * 128},
	{260, 1, 5 * 128}, {261, 1, 6 * 128}, {262, 1, 7 * 128}, {263, 1, 8 * 128},
	{264, 1, 9 * 128}, {265, 1, 10 * 128}, {266, 1, 11 * 128}, {267, 1, 12 * 128},
	{268, 1, 13 * 128}, {269, 1, 14 * 128}, {270, 1, 15 * 128}, {271, 1, 16 * 128},
	{272, 1, 17 * 128}, {273, 1, 18 * 128}, {274, 1, 19 * 128}, {275, 1, 20 * 128},
	{276, 1, 21 * 128}, {277, 1, 22 * 128}, {278, 1, 23 * 128}, {279, 1, 24 * 128},
	{280, 1, 25 * 128}, {281, 1, 26 * 128}, {282, 1, 27 * 128}, {283, 1, 28 * 128},
	{284, 1, 29 * 128}, {285, 1, 30 * 128}, {286, 1, 31 * 128}, {287, 1, 32 * 128},

	{0, 0},
};

static struct clk_div_table apb_div_table[] = {
	{1, 2},   {2, 3},   {3, 4},
	{0, 0},
};

static struct clk_div_table eth_mac_div_table[] = {
	{0, 2},   {1, 4},
	{0, 0},
};

static struct clk_div_table rmii_ref_div_table[] = {
	{0, 4},   {1, 10},
	{0, 0},
};

static struct clk_div_table usb3_mac_div_table[] = {
	{1, 2},   {2, 3},   {3, 4},
	{0, 8},
};

static struct clk_div_table i2s_div_table[] = {
	{0, 1},   {1, 2},   {2, 3},   {3, 4},
	{4, 6},   {5, 8},   {6, 12},  {7, 16},
	{8, 24},
	{0, 0},
};

static struct clk_div_table hdmia_div_table[] = {
	{0, 1},   {1, 2},   {2, 3},   {3, 4},
	{4, 6},   {5, 8},   {6, 12},  {7, 16},
	{8, 24},
	{0, 0},
};


/* divider clocks */
static struct owl_divider_clock s900_div_clks[] = {
	{ CLK_NOC_CLK_DIV, "noc_clk_div", "noc_clk", 0, CMU_BUSCLK, 19, 1, 0, NULL, },
	{ CLK_AHB, "ahb_clk", "noc_clk_div", 0, CMU_BUSCLK, 4, 1, 0, NULL, "ahb_clk", },
	{ CLK_APB, "apb_clk", "ahb_clk", 0, CMU_BUSCLK, 8, 2, 0, apb_div_table, "apb_clk", },
	{ CLK_USB3_MAC, "usb3_mac", "assist_pll", 0, CMU_ASSISTPLL, 12, 2, 0, usb3_mac_div_table, "usb3_mac", },
	{ CLK_RMII_REF, "rmii_ref", "assist_pll", 0, CMU_ASSISTPLL, 8, 1, 0, rmii_ref_div_table, "rmii_ref", },
};

static struct clk_factor_table dmm_factor_table[] = {
	{0, 1, 1}, {1, 2, 3}, {2, 1, 2}, {3, 1, 3},
	{4, 1, 4},
	{0, 0, 0},
};

static struct clk_factor_table noc_factor_table[] = {
	{0, 1, 1},   {1, 2, 3},   {2, 1, 2}, {3, 1, 3},  {4, 1, 4},
	{0, 0, 0},
};

static struct clk_factor_table bisp_factor_table[] = {
	{0, 1, 1}, {1, 2, 3}, {2, 1, 2}, {3, 2, 5},
	{4, 1, 3}, {5, 1, 4}, {6, 1, 6}, {7, 1, 8},
	{0, 0, 0},
};

/* divider clocks */
static struct owl_factor_clock s900_factor_clks[] = {
	{ CLK_NOC, "noc_clk", "noc_clk_mux", 0, CMU_BUSCLK, 16, 3, 0, noc_factor_table, "noc_clk", },
	{ CLK_DE1, "de_clk1", "de_clk", 0, CMU_DECLK, 0, 3, 0, bisp_factor_table, "de_clk1", },
	{ CLK_DE2, "de_clk2", "de_clk", 0, CMU_DECLK, 4, 3, 0, bisp_factor_table, "de_clk2", },
	{ CLK_DE3, "de_clk3", "de_clk", 0, CMU_DECLK, 8, 3, 0, bisp_factor_table, "de_clk3", },
};

/* gate clocks */
static struct owl_gate_clock s900_gate_clks[] = {
	{ CLK_GPIO,  "gpio", "apb_clk", 0, CMU_DEVCLKEN0, 18, 0, "gpio", },
	{ CLK_GPU,   "gpu", NULL, 0, CMU_DEVCLKEN0, 30, 0, "gpu", },
	{ CLK_DMAC,  "dmac", "noc_clk_div", 0, CMU_DEVCLKEN0, 1, 0, "dmac", },
	{ CLK_TIMER,  "timer", "hosc", 0, CMU_DEVCLKEN1, 27, 0, "timer", },
	{ CLK_DSI,  "dsi_clk", NULL, 0, CMU_DEVCLKEN0, 12, 0, "dsi", },

	{ CLK_DDR0,  "ddr0_clk", "ddr_pll", CLK_IGNORE_UNUSED, CMU_DEVCLKEN0, 31, 0, "ddr0", },
	{ CLK_DDR1,  "ddr1_clk", "ddr_pll", CLK_IGNORE_UNUSED, CMU_DEVCLKEN0, 29, 0, "ddr1", },

	{ CLK_USB3_480MPLL0,	"usb3_480mpll0",	NULL, 0, CMU_USBPLL, 3, 0, "usb3_480mpll0", },
	{ CLK_USB3_480MPHY0,	"usb3_480mphy0",	NULL, 0, CMU_USBPLL, 2, 0, "usb3_480mphy0", },
	{ CLK_USB3_5GPHY,	"usb3_5gphy",		NULL, 0, CMU_USBPLL, 1, 0, "usb3_5gphy", },
	{ CLK_USB3_CCE,		"usb3_cce",		NULL, 0, CMU_USBPLL, 0, 0, "usb3_cce", },

	{ CLK_24M_EDP,		"24M_edp",		"diff24M", 0, CMU_EDPCLK, 8, 0, "24M_edp", },
	{ CLK_EDP_LINK,		"edp_link",		"edp_pll", 0, CMU_DEVCLKEN0, 10, 0, "edp_link", },

	{ CLK_USB2H0_PLLEN,	"usbh0_pllen",	NULL, 0, CMU_USBPLL, 12, 0, "usbh0_pllen", },
	{ CLK_USB2H0_PHY,	"usbh0_phy",	NULL, 0, CMU_USBPLL, 10, 0, "usbh0_phy", },
	{ CLK_USB2H0_CCE,	"usbh0_cce",		NULL, 0, CMU_USBPLL, 8, 0, "usbh0_cce", },

	{ CLK_USB2H1_PLLEN,	"usbh1_pllen",	NULL, 0, CMU_USBPLL, 13, 0, "usbh1_pllen", },
	{ CLK_USB2H1_PHY,	"usbh1_phy",	NULL, 0, CMU_USBPLL, 11, 0, "usbh1_phy", },
	{ CLK_USB2H1_CCE,	"usbh1_cce",		NULL, 0, CMU_USBPLL, 9, 0, "usbh1_cce", },
};

static struct owl_composite_clock s900_composite_clks[] = {
	COMP_FACTOR_CLK(CLK_BISP, "bisp", 0,
			C_MUX(bisp_clk_mux_p, CMU_BISPCLK, 4, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 14, 0),
			C_FACTOR(CMU_BISPCLK, 0, 3, bisp_factor_table, 0)),

	COMP_DIV_CLK(CLK_CSI0, "csi0", 0,
			C_MUX(csi_clk_mux_p, CMU_CSICLK, 4, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 13, 0),
			C_DIVIDER(CMU_CSICLK, 0, 4, NULL, 0)),

	COMP_DIV_CLK(CLK_CSI1, "csi1", 0,
			C_MUX(csi_clk_mux_p, CMU_CSICLK, 20, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 15, 0),
			C_DIVIDER(CMU_CSICLK, 16, 4, NULL, 0)),

	COMP_PASS_CLK(CLK_DE, "de_clk", 0,
			C_MUX(de_clk_mux_p, CMU_DECLK, 12, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 8, 0)),

	COMP_FACTOR_CLK(CLK_DMM, "dmm", CLK_IGNORE_UNUSED,
			C_MUX(dmm_clk_mux_p, CMU_BUSCLK, 10, 2, 0),
			C_GATE(CMU_DEVCLKEN0, 19, 0),
			C_FACTOR(CMU_BUSCLK, 12, 3, dmm_factor_table, 0)),

	COMP_FACTOR_CLK(CLK_EDP, "edp_clk", 0,
			C_MUX(edp_clk_mux_p, CMU_EDPCLK, 19, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 10, 0),
			C_FACTOR(CMU_EDPCLK, 16, 3, bisp_factor_table, 0)),

	COMP_DIV_CLK(CLK_ETH_MAC, "eth_mac", 0,
			C_MUX_F(eth_mac_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 22, 0),
			C_DIVIDER(CMU_ASSISTPLL, 10, 1, eth_mac_div_table, 0)),

	COMP_FACTOR_CLK(CLK_GPU_CORE, "gpu_core", 0,
			C_MUX(gpu_clk_mux_p, CMU_GPU3DCLK, 4, 2, 0),
			C_GATE(CMU_GPU3DCLK, 15, 0),
			C_FACTOR(CMU_GPU3DCLK, 0, 3, bisp_factor_table, 0)),

	COMP_FACTOR_CLK(CLK_GPU_MEM, "gpu_mem", 0,
			C_MUX(gpu_clk_mux_p, CMU_GPU3DCLK, 20, 2, 0),
			C_GATE(CMU_GPU3DCLK, 14, 0),
			C_FACTOR(CMU_GPU3DCLK, 16, 3, bisp_factor_table, 0)),

	COMP_FACTOR_CLK(CLK_GPU_SYS, "gpu_sys", 0,
			C_MUX(gpu_clk_mux_p, CMU_GPU3DCLK, 28, 2, 0),
			C_GATE(CMU_GPU3DCLK, 13, 0),
			C_FACTOR(CMU_GPU3DCLK, 24, 3, bisp_factor_table, 0)),

	COMP_FACTOR_CLK(CLK_HDE, "hde", 0,
			C_MUX(hde_clk_mux_p, CMU_HDECLK, 4, 2, 0),
			C_GATE(CMU_DEVCLKEN0, 27, 0),
			C_FACTOR(CMU_HDECLK, 0, 3, bisp_factor_table, 0)),

	COMP_DIV_CLK(CLK_HDMI_AUDIO, "hdmia", 0,
			C_MUX(i2s_clk_mux_p, CMU_AUDIOPLL, 24, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 22, 0),
			C_DIVIDER(CMU_AUDIOPLL, 24, 4, hdmia_div_table, 0)),

	COMP_FIXED_FACTOR_CLK(CLK_I2C0, "i2c0", 0,
			C_MUX_F(i2c_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 14, 0),
			C_FIXED_FACTOR(1, 5)),

	COMP_FIXED_FACTOR_CLK(CLK_I2C1, "i2c1", 0,
			C_MUX_F(i2c_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 15, 0),
			C_FIXED_FACTOR(1, 5)),

	COMP_FIXED_FACTOR_CLK(CLK_I2C2, "i2c2", 0,
			C_MUX_F(i2c_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 30, 0),
			C_FIXED_FACTOR(1, 5)),

	COMP_FIXED_FACTOR_CLK(CLK_I2C3, "i2c3", 0,
			C_MUX_F(i2c_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 31, 0),
			C_FIXED_FACTOR(1, 5)),

	COMP_FIXED_FACTOR_CLK(CLK_I2C4, "i2c4", 0,
			C_MUX_F(i2c_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN0, 17, 0),
			C_FIXED_FACTOR(1, 5)),

	COMP_FIXED_FACTOR_CLK(CLK_I2C5, "i2c5", 0,
			C_MUX_F(i2c_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 1, 0),
			C_FIXED_FACTOR(1, 5)),

	COMP_DIV_CLK(CLK_I2SRX, "i2srx", 0,
			C_MUX(i2s_clk_mux_p, CMU_AUDIOPLL, 24, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 21, 0),
			C_DIVIDER(CMU_AUDIOPLL, 20, 4, i2s_div_table, 0)),

	COMP_DIV_CLK(CLK_I2STX, "i2stx", 0,
			C_MUX(i2s_clk_mux_p, CMU_AUDIOPLL, 24, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 20, 0),
			C_DIVIDER(CMU_AUDIOPLL, 16, 4, i2s_div_table, 0)),

	COMP_FACTOR_CLK(CLK_IMX, "imx", 0,
			C_MUX(imx_clk_mux_p, CMU_IMXCLK, 4, 1, 0),
			C_GATE(CMU_DEVCLKEN1, 17, 0),
			C_FACTOR(CMU_IMXCLK, 0, 3, bisp_factor_table, 0)),

	COMP_DIV_CLK(CLK_LCD, "lcd", 0,
			C_MUX(lcd_clk_mux_p, CMU_LCDCLK, 12, 2, 0),
			C_GATE(CMU_DEVCLKEN0, 9, 0),
			C_DIVIDER(CMU_LCDCLK, 0, 5, NULL, 0)),

	COMP_DIV_CLK(CLK_NAND0, "nand0", CLK_SET_RATE_PARENT,
			C_MUX(nand_clk_mux_p, CMU_NANDCCLK, 8, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 4, 0),
			C_DIVIDER(CMU_NANDCCLK, 0, 4, nand_div_table, 0)),

	COMP_DIV_CLK(CLK_NAND1, "nand1", CLK_SET_RATE_PARENT,
			C_MUX(nand_clk_mux_p, CMU_NANDCCLK, 24, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 11, 0),
			C_DIVIDER(CMU_NANDCCLK, 16, 4, nand_div_table, 0)),

	COMP_DIV_CLK(CLK_PWM0, "pwm0", 0,
			C_MUX_F(pwm_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 23, 0),
			C_DIVIDER(CMU_PWM0CLK, 0, 6, NULL, 0)),

	COMP_DIV_CLK(CLK_PWM0, "pwm1", 0,
			C_MUX_F(pwm_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 24, 0),
			C_DIVIDER(CMU_PWM1CLK, 0, 6, NULL, 0)),
	/*
	 * pwm2 may be for backlight, do not gate it
	 * even it is "unused", because it may be
	 * enabled at boot stage, and in kernel, driver
	 * has no effective method to know the real status,
	 * so, the best way is keeping it as what it was.
	 */
	COMP_DIV_CLK(CLK_PWM0, "pwm2", CLK_IGNORE_UNUSED,
			C_MUX_F(pwm_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 25, 0),
			C_DIVIDER(CMU_PWM2CLK, 0, 6, NULL, 0)),

	COMP_DIV_CLK(CLK_PWM0, "pwm3", 0,
			C_MUX_F(pwm_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 26, 0),
			C_DIVIDER(CMU_PWM3CLK, 0, 6, NULL, 0)),

	COMP_DIV_CLK(CLK_PWM0, "pwm4", 0,
			C_MUX_F(pwm_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 4, 0),
			C_DIVIDER(CMU_PWM4CLK, 0, 6, NULL, 0)),

	COMP_DIV_CLK(CLK_PWM5, "pwm5", 0,
			C_MUX_F(pwm_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 5, 0),
			C_DIVIDER(CMU_PWM5CLK, 0, 6, NULL, 0)),

	COMP_FACTOR_CLK(CLK_SD0, "sd0", 0,
			C_MUX(sd_clk_mux_p, CMU_SD0CLK, 9, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 5, 0),
			C_FACTOR(CMU_SD0CLK, 0, 9, sd_factor_table, 0)),

	COMP_FACTOR_CLK(CLK_SD1, "sd1", 0,
			C_MUX(sd_clk_mux_p, CMU_SD1CLK, 9, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 6, 0),
			C_FACTOR(CMU_SD1CLK, 0, 9, sd_factor_table, 0)),

	COMP_FACTOR_CLK(CLK_SD2, "sd2", 0,
			C_MUX(sd_clk_mux_p, CMU_SD2CLK, 9, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 7, 0),
			C_FACTOR(CMU_SD2CLK, 0, 9, sd_factor_table, 0)),

	COMP_FACTOR_CLK(CLK_SD3, "sd3", 0,
			C_MUX(sd_clk_mux_p, CMU_SD3CLK, 9, 1, 0),
			C_GATE(CMU_DEVCLKEN0, 16, 0),
			C_FACTOR(CMU_SD3CLK, 0, 9, sd_factor_table, 0)),

	COMP_DIV_CLK(CLK_SENSOR, "sensor", 0,
			C_MUX(sensor_clk_mux_p, CMU_SENSORCLK, 4, 1, 0),
			C_NULL,
			C_DIVIDER(CMU_SENSORCLK, 0, 4, NULL, 0)),

	COMP_DIV_CLK(CLK_SPEED_SENSOR, "speed_sensor", 0,
			C_MUX_F(speed_sensor_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 0, 0),
			C_DIVIDER(CMU_TLSCLK, 0, 4, NULL, CLK_DIVIDER_POWER_OF_TWO)),

	COMP_PASS_CLK(CLK_SPI0, "spi0", 0,
			C_MUX_F(spi_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 10, 0)),

	COMP_PASS_CLK(CLK_SPI1, "spi1", 0,
			C_MUX_F(spi_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 11, 0)),

	COMP_PASS_CLK(CLK_SPI2, "spi2", 0,
			C_MUX_F(spi_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 12, 0)),

	COMP_PASS_CLK(CLK_SPI3, "spi3", 0,
			C_MUX_F(spi_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 13, 0)),

	COMP_DIV_CLK(CLK_THERMAL_SENSOR, "thermal_sensor", 0,
			C_MUX_F(thermal_sensor_clk_mux_p, 0),
			C_GATE(CMU_DEVCLKEN1, 2, 0),
			C_DIVIDER(CMU_TLSCLK, 8, 4, NULL, CLK_DIVIDER_POWER_OF_TWO)),

	COMP_DIV_CLK(CLK_UART0, "uart0", 0,
			C_MUX(uart_clk_mux_p, CMU_UART0CLK, 16, 1, 0),
			C_GATE(CMU_DEVCLKEN1, 6, 0),
			C_DIVIDER(CMU_UART0CLK, 0, 8, NULL, CLK_DIVIDER_ROUND_CLOSEST)),

	COMP_DIV_CLK(CLK_UART1, "uart1", 0,
			C_MUX(uart_clk_mux_p, CMU_UART1CLK, 16, 1, 0),
			C_GATE(CMU_DEVCLKEN1, 7, 0),
			C_DIVIDER(CMU_UART1CLK, 1, 8, NULL, CLK_DIVIDER_ROUND_CLOSEST)),

	COMP_DIV_CLK(CLK_UART2, "uart2", 0,
			C_MUX(uart_clk_mux_p, CMU_UART2CLK, 16, 1, 0),
			C_GATE(CMU_DEVCLKEN1, 8, 0),
			C_DIVIDER(CMU_UART2CLK, 0, 8, NULL, CLK_DIVIDER_ROUND_CLOSEST)),

	COMP_DIV_CLK(CLK_UART3, "uart3", 0,
			C_MUX(uart_clk_mux_p, CMU_UART3CLK, 16, 1, 0),
			C_GATE(CMU_DEVCLKEN1, 19, 0),
			C_DIVIDER(CMU_UART3CLK, 0, 8, NULL, CLK_DIVIDER_ROUND_CLOSEST)),

	COMP_DIV_CLK(CLK_UART4, "uart4", 0,
			C_MUX(uart_clk_mux_p, CMU_UART4CLK, 16, 1, 0),
			C_GATE(CMU_DEVCLKEN1, 20, 0),
			C_DIVIDER(CMU_UART4CLK, 0, 8, NULL, CLK_DIVIDER_ROUND_CLOSEST)),

	COMP_DIV_CLK(CLK_UART5, "uart5", 0,
			C_MUX(uart_clk_mux_p, CMU_UART5CLK, 16, 1, 0),
			C_GATE(CMU_DEVCLKEN1, 21, 0),
			C_DIVIDER(CMU_UART5CLK, 0, 8, NULL, CLK_DIVIDER_ROUND_CLOSEST)),

	COMP_DIV_CLK(CLK_UART6, "uart6", 0,
			C_MUX(uart_clk_mux_p, CMU_UART6CLK, 16, 1, 0),
			C_GATE(CMU_DEVCLKEN1, 18, 0),
			C_DIVIDER(CMU_UART6CLK, 0, 8, NULL, CLK_DIVIDER_ROUND_CLOSEST)),

	COMP_FACTOR_CLK(CLK_VCE, "vce", 0,
			C_MUX(vce_clk_mux_p, CMU_VCECLK, 4, 2, 0),
			C_GATE(CMU_DEVCLKEN0, 26, 0),
			C_FACTOR(CMU_VCECLK, 0, 3, bisp_factor_table, 0)),

	COMP_FACTOR_CLK(CLK_VDE, "vde", 0,
			C_MUX(hde_clk_mux_p, CMU_VDECLK, 4, 2, 0),
			C_GATE(CMU_DEVCLKEN0, 25, 0),
			C_FACTOR(CMU_VDECLK, 0, 3, bisp_factor_table, 0)),
};

static int s900_clk_probe(struct platform_device *pdev)
{
	struct owl_clk_provider *ctx;
	struct device_node *np = pdev->dev.of_node;
	struct resource *res;
	void __iomem *base;
	int i;

	ctx = kzalloc(sizeof(struct owl_clk_provider) +
			sizeof(*ctx->clk_data.hws) * CLK_NR_CLKS, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	for (i = 0; i < CLK_NR_CLKS; ++i)
		ctx->clk_data.hws[i] = ERR_PTR(-ENOENT);

	ctx->reg_base = base;
	ctx->clk_data.num = CLK_NR_CLKS;
	spin_lock_init(&ctx->lock);

	/* register pll clocks */
	owl_clk_register_pll(ctx, s900_pll_clks,
			ARRAY_SIZE(s900_pll_clks));

	/* register divider clocks */
	owl_clk_register_divider(ctx, s900_div_clks,
			ARRAY_SIZE(s900_div_clks));

	/* register factor divider clocks */
	owl_clk_register_factor(ctx, s900_factor_clks,
			ARRAY_SIZE(s900_factor_clks));

	/* register mux clocks */
	owl_clk_register_mux(ctx, s900_mux_clks,
			ARRAY_SIZE(s900_mux_clks));

	/* register gate clocks */
	owl_clk_register_gate(ctx, s900_gate_clks,
			ARRAY_SIZE(s900_gate_clks));

	/* register composite clocks */
	owl_clk_register_composite(ctx, s900_composite_clks,
			ARRAY_SIZE(s900_composite_clks));

	return of_clk_add_hw_provider(np, of_clk_hw_onecell_get,
				&ctx->clk_data);

}

static const struct of_device_id s900_clk_of_match[] = {
	{ .compatible = "actions,s900-cmu", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s900_clk_of_match);

static struct platform_driver s900_clk_driver = {
	.probe = s900_clk_probe,
	.driver = {
		.name = "s900-cmu",
		.of_match_table = s900_clk_of_match,
	},
};

static int __init s900_clk_init(void)
{
	return platform_driver_register(&s900_clk_driver);
}
core_initcall(s900_clk_init);
