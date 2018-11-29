/* SPDX-License-Identifier: (GPL-2.0+ OR MIT) */
/*
 * Copyright (c) 2016 Amlogic, Inc.
 * Author: Michael Turquette <mturquette@baylibre.com>
 *
 * Copyright (c) 2018 Amlogic, inc.
 * Author: Qiufang Dai <qiufang.dai@amlogic.com>
 * Author: Jian Hu <jian.hu@amlogic.com>
 *
 */
#ifndef __G12A_H
#define __G12A_H

/*
 * Clock controller register offsets
 *
 * Register offsets from the data sheet must be multiplied by 4 before
 * adding them to the base address to get the right value.
 */
#define HHI_MIPI_CNTL0			0x000
#define HHI_MIPI_CNTL1			0x004
#define HHI_MIPI_CNTL2			0x008
#define HHI_MIPI_STS			0x00C
#define HHI_GP0_PLL_CNTL0		0x040
#define HHI_GP0_PLL_CNTL1		0x044
#define HHI_GP0_PLL_CNTL2		0x048
#define HHI_GP0_PLL_CNTL3		0x04C
#define HHI_GP0_PLL_CNTL4		0x050
#define HHI_GP0_PLL_CNTL5		0x054
#define HHI_GP0_PLL_CNTL6		0x058
#define HHI_GP0_PLL_STS			0x05C
#define HHI_PCIE_PLL_CNTL0		0x098
#define HHI_PCIE_PLL_CNTL1		0x09C
#define HHI_PCIE_PLL_CNTL2		0x0A0
#define HHI_PCIE_PLL_CNTL3		0x0A4
#define HHI_PCIE_PLL_CNTL4		0x0A8
#define HHI_PCIE_PLL_CNTL5		0x0AC
#define HHI_PCIE_PLL_STS		0x0B8
#define HHI_HIFI_PLL_CNTL0		0x0D8
#define HHI_HIFI_PLL_CNTL1		0x0DC
#define HHI_HIFI_PLL_CNTL2		0x0E0
#define HHI_HIFI_PLL_CNTL3		0x0E4
#define HHI_HIFI_PLL_CNTL4		0x0E8
#define HHI_HIFI_PLL_CNTL5		0x0EC
#define HHI_HIFI_PLL_CNTL6		0x0F0
#define HHI_GCLK_MPEG0			0x140
#define HHI_GCLK_MPEG1			0x144
#define HHI_GCLK_MPEG2			0x148
#define HHI_GCLK_OTHER			0x150
#define HHI_MPEG_CLK_CNTL		0x174
#define HHI_AUD_CLK_CNTL		0x178
#define HHI_VID_CLK_CNTL		0x17c
#define HHI_TS_CLK_CNTL			0x190
#define HHI_VID_CLK_CNTL2		0x194
#define HHI_SYS_CPU_CLK_CNTL0		0x19c
#define HHI_MALI_CLK_CNTL		0x1b0
#define HHI_VPU_CLKC_CNTL		0x1b4
#define HHI_VPU_CLK_CNTL		0x1bC
#define HHI_HDMI_CLK_CNTL		0x1CC
#define HHI_VDEC_CLK_CNTL		0x1E0
#define HHI_VDEC2_CLK_CNTL		0x1E4
#define HHI_VDEC3_CLK_CNTL		0x1E8
#define HHI_VDEC4_CLK_CNTL		0x1EC
#define HHI_HDCP22_CLK_CNTL		0x1F0
#define HHI_VAPBCLK_CNTL		0x1F4
#define HHI_VPU_CLKB_CNTL		0x20C
#define HHI_GEN_CLK_CNTL		0x228
#define HHI_VDIN_MEAS_CLK_CNTL		0x250
#define HHI_MIPIDSI_PHY_CLK_CNTL	0x254
#define HHI_NAND_CLK_CNTL		0x25C
#define HHI_SD_EMMC_CLK_CNTL		0x264
#define HHI_MPLL_CNTL0			0x278
#define HHI_MPLL_CNTL1			0x27C
#define HHI_MPLL_CNTL2			0x280
#define HHI_MPLL_CNTL3			0x284
#define HHI_MPLL_CNTL4			0x288
#define HHI_MPLL_CNTL5			0x28c
#define HHI_MPLL_CNTL6			0x290
#define HHI_MPLL_CNTL7			0x294
#define HHI_MPLL_CNTL8			0x298
#define HHI_FIX_PLL_CNTL0		0x2A0
#define HHI_FIX_PLL_CNTL1		0x2A4
#define HHI_SYS_PLL_CNTL0		0x2f4
#define HHI_SYS_PLL_CNTL1		0x2f8
#define HHI_SYS_PLL_CNTL2		0x2fc
#define HHI_SYS_PLL_CNTL3		0x300
#define HHI_SYS_PLL_CNTL4		0x304
#define HHI_SYS_PLL_CNTL5		0x308
#define HHI_SYS_PLL_CNTL6		0x30c
#define HHI_SPICC_CLK_CNTL		0x3dc

/*
 * CLKID index values
 *
 * These indices are entirely contrived and do not map onto the hardware.
 * It has now been decided to expose everything by default in the DT header:
 * include/dt-bindings/clock/g12a-clkc.h. Only the clocks ids we don't want
 * to expose, such as the internal muxes and dividers of composite clocks,
 * will remain defined here.
 */
#define CLKID_MPEG_SEL				8
#define CLKID_MPEG_DIV				9
#define CLKID_SD_EMMC_B_CLK0_SEL		62
#define CLKID_SD_EMMC_B_CLK0_DIV		63
#define CLKID_SD_EMMC_C_CLK0_SEL		64
#define CLKID_SD_EMMC_C_CLK0_DIV		65
#define CLKID_MPLL0_DIV				66
#define CLKID_MPLL1_DIV				67
#define CLKID_MPLL2_DIV				68
#define CLKID_MPLL3_DIV				69
#define CLKID_MPLL_PREDIV			70
#define CLKID_FCLK_DIV2_DIV			72
#define CLKID_FCLK_DIV3_DIV			73
#define CLKID_FCLK_DIV4_DIV			74
#define CLKID_FCLK_DIV5_DIV			75
#define CLKID_FCLK_DIV7_DIV			76
#define CLKID_FCLK_DIV2P5_DIV			97
#define CLKID_FIXED_PLL_DCO			98
#define CLKID_SYS_PLL_DCO			99
#define CLKID_GP0_PLL_DCO			100
#define CLKID_HIFI_PLL_DCO			101
/* CLKID_EE_XTAL equal NR_CLKS plus one */
#define CLKID_EE_CORE				103

#define NR_CLKS					102

/* include the CLKIDs that have been made part of the DT binding */
#include <dt-bindings/clock/g12a-clkc.h>

#endif /* __G12A_H */
