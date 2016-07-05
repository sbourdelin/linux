/*
 * Copyright (c) 2016 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com/
 *
 * EXYNOS7 - CPU PMU (Power Management Unit) support
 * Author: Abhilash Kesavan <a.kesavan@samsung.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/of_address.h>
#include <linux/soc/samsung/exynos-regs-pmu.h>
#include <linux/soc/samsung/exynos-pmu.h>

#include "exynos-pmu.h"

static const struct exynos_pmu_conf exynos7_pmu_config[] = {
	/* { .offset = offset, .val = { AFTR, LPA, SLEEP } } */
	{ EXYNOS7_ATLAS_CPU0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU0_LOCAL_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU0_CENTRAL_SYS_PWR_REG, { 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU0_CPUSEQ_SYS_PWR_REG, { 0x0, 0x0, 0x0 } },
	{ EXYNOS7_ATLAS_CPU1_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU1_LOCAL_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU1_CENTRAL_SYS_PWR_REG, { 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU1_CPUSEQ_SYS_PWR_REG, { 0x0, 0x0, 0x0 } },
	{ EXYNOS7_ATLAS_CPU2_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU2_LOCAL_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU2_CENTRAL_SYS_PWR_REG, { 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU2_CPUSEQ_SYS_PWR_REG, { 0x0, 0x0, 0x0 } },
	{ EXYNOS7_ATLAS_CPU3_SYS_PWR_REG,		{ 0x0, 0x0, 0x8 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU3_LOCAL_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU3_CENTRAL_SYS_PWR_REG, { 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DIS_IRQ_ATLAS_CPU3_CPUSEQ_SYS_PWR_REG, { 0x0, 0x0, 0x0 } },
	{ EXYNOS7_ATLAS_NONCPU_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_ATLAS_DBG_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_ATLAS_L2_SYS_PWR_REG,			{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_TOP_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_TOP_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_TOP_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CPUCLKSTOP_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_MIF_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_MIF_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_MIF_SYS_PWR_REG,		{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_DDRPHY_DLLLOCK_SYS_PWR_REG,		{ 0x1, 0x1, 0x1 } },
	{ EXYNOS7_DISABLE_PLL_CMU_TOP_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_MIF_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_TOP_BUS_SYS_PWR_REG,			{ 0x7, 0x0, 0x0 } },
	{ EXYNOS7_TOP_RETENTION_SYS_PWR_REG,		{ 0x1, 0x0, 0x1 } },
	{ EXYNOS7_TOP_PWR_SYS_PWR_REG,			{ 0x3, 0x0, 0x3 } },
	{ EXYNOS7_TOP_BUS_MIF_SYS_PWR_REG,		{ 0x7, 0x0, 0x0 } },
	{ EXYNOS7_TOP_RETENTION_MIF_SYS_PWR_REG,	{ 0x1, 0x0, 0x1 } },
	{ EXYNOS7_TOP_PWR_MIF_SYS_PWR_REG,		{ 0x3, 0x0, 0x3 } },
	{ EXYNOS7_RET_OSCCLK_GATE_SYS_PWR_REG,		{ 0x1, 0x0, 0x1 } },
	{ EXYNOS7_LOGIC_RESET_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_OSCCLK_GATE_SYS_PWR_REG,		{ 0x1, 0x0, 0x1 } },
	{ EXYNOS7_SLEEP_RESET_SYS_PWR_REG,		{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_LOGIC_RESET_MIF_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_OSCCLK_GATE_MIF_SYS_PWR_REG,		{ 0x1, 0x0, 0x1 } },
	{ EXYNOS7_SLEEP_RESET_MIF_SYS_PWR_REG,		{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_RET_OSCCLK_GATE_MIF_SYS_PWR_REG,	{ 0x1, 0x0, 0x1 } },
	{ EXYNOS7_MEMORY_TOP_SYS_PWR_REG,		{ 0x3, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_TOP_ALV_SYS_PWR_REG,		{ 0x3, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_LPDDR4_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_AUD_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_JTAG_SYS_PWR_REG,	{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_MMC2_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_TOP_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_UART_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_MMC0_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_MMC1_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_EBIA_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_EBIB_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_SPI_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_MIF_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_ISOLATION_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_LLI_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_UFS_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_PAD_ISOLATION_MIF_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_RETENTION_FSYSGENIO_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_PAD_ALV_SEL_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_XXTI_SYS_PWR_REG,			{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_XXTI26_SYS_PWR_REG,			{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_EXT_REGULATOR_SYS_PWR_REG,		{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_GPIO_MODE_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_GPIO_MODE_FSYS0_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_GPIO_MODE_FSYS1_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_GPIO_MODE_BUS0_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_GPIO_MODE_MIF_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_GPIO_MODE_AUD_SYS_PWR_REG,		{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_ATLAS_SYS_PWR_REG,			{ 0xF, 0xF, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_ATLAS_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_ATLAS_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_ATLAS_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_ATLAS_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_ATLAS_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_ATLAS_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_SLEEP_ATLAS_SYS_PWR_REG,	{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_AUD_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_BUS0_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_CAM0_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_CAM1_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_DISP_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_FSYS0_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_FSYS1_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_G3D_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_ISP0_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_ISP1_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_MFC_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_MSCL_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_VPP_SYS_PWR_REG,			{ 0xF, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_AUD_SYS_PWR_REG,		{ 0x0, 0x1, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_BUS0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_DISP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_FSYS0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_FSYS1_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_G3D_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_ISP0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_ISP1_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_MFC_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_MSCL_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKRUN_CMU_VPP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_AUD_SYS_PWR_REG,		{ 0x0, 0x1, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_BUS0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_DISP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_FSYS0_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_FSYS1_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_G3D_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_ISP0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_ISP1_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_MFC_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_MSCL_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_CLKSTOP_CMU_VPP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_AUD_SYS_PWR_REG,	{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_BUS0_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_DISP_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_FSYS0_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_FSYS1_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_G3D_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_ISP0_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_ISP1_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_MFC_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_MSCL_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_DISABLE_PLL_CMU_VPP_SYS_PWR_REG,	{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_AUD_SYS_PWR_REG,		{ 0x0, 0x1, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_BUS0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_DISP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_FSYS0_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_FSYS1_SYS_PWR_REG,	{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_G3D_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_ISP0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_ISP1_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_MFC_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_MSCL_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_LOGIC_VPP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_AUD_SYS_PWR_REG,		{ 0x0, 0x3, 0x0 } },
	{ EXYNOS7_MEMORY_DISP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_FSYS0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_FSYS1_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_G3D_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_ISP0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_ISP1_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_MFC_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_MSCL_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_MEMORY_VPP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_AUD_SYS_PWR_REG,		{ 0x0, 0x1, 0x0 } },
	{ EXYNOS7_RESET_CMU_BUS0_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_DISP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_FSYS0_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_FSYS1_SYS_PWR_REG,		{ 0x1, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_G3D_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_ISP0_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_ISP1_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_MFC_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_MSCL_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_CMU_VPP_SYS_PWR_REG,		{ 0x0, 0x0, 0x0 } },
	{ EXYNOS7_RESET_SLEEP_BUS0_SYS_PWR_REG,		{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_RESET_SLEEP_FSYS0_SYS_PWR_REG,	{ 0x1, 0x1, 0x0 } },
	{ EXYNOS7_RESET_SLEEP_FSYS1_SYS_PWR_REG,	{ 0x1, 0x1, 0x0 } },
	{ PMU_TABLE_END, },
};

/* PMU configurations (provided by hardware team) that are not part of the UM */
static const struct exynos_pmu_conf exynos7_pmu_config_extra[] = {
	/* { .offset = offset, .val = { AFTR, LPA, SLEEP } } */
	{ EXYNOS7_PMU_SYNC_CTRL,	{ 0x0,      0x0,        0x0        } },
	{ EXYNOS7_CENTRAL_SEQ_MIF_OPTION, { 0x1000, 0x1000,     0x0        } },
	{ EXYNOS7_WAKEUP_MASK_MIF,	{ 0x100013, 0x100013,   0x0        } },
	{ EXYNOS7_ATLAS_NONCPU_OPTION,	{ 0x11,     0x11,       0x11       } },
	{ EXYNOS7_MEMORY_TOP_OPTION,	{ 0x11,     0x11,       0x1        } },
	{ EXYNOS7_MEMORY_TOP_ALV_OPTION, { 0x11,    0x11,       0x11       } },
	{ EXYNOS7_RESET_CMU_TOP_OPTION,	{ 0x0,      0x80000000, 0x0        } },
	{ EXYNOS7_ATLAS_OPTION,		{ 0x101,    0x101,      0x80001101 } },
	{ EXYNOS7_BUS0_OPTION,		{ 0x101,    0x101,      0x1101     } },
	{ EXYNOS7_FSYS0_OPTION,		{ 0x101,    0x101,      0x1101     } },
	{ EXYNOS7_FSYS1_OPTION,		{ 0x101,    0x101,      0x1101     } },
	{ EXYNOS7_AUD_OPTION,		{ 0x101,    0xC0000101, 0x101      } },
	{ EXYNOS7_G3D_OPTION,		{ 0x181,    0x181,	0x181      } },
	{ EXYNOS7_SLEEP_RESET_OPTION,	{ 0x100000, 0x100000,   0x100000   } },
	{ EXYNOS7_TOP_PWR_OPTION,	{ 0x1,      0x80800002, 0x1        } },
	{ EXYNOS7_TOP_PWR_MIF_OPTION,	{ 0x1,      0x1,	0x1        } },
	{ EXYNOS7_LOGIC_RESET_OPTION,	{ 0x0,      0x80000000, 0x0        } },
	{ EXYNOS7_TOP_RETENTION_OPTION,	{ 0x0,      0x80000000, 0x0        } },
	{ PMU_TABLE_END, },
};

static unsigned int const exynos7_list_feed[] = {
	EXYNOS7_ATLAS_NONCPU_OPTION,
	EXYNOS7_TOP_PWR_OPTION,
	EXYNOS7_TOP_PWR_MIF_OPTION,
	EXYNOS7_AUD_OPTION,
	EXYNOS7_CAM0_OPTION,
	EXYNOS7_DISP_OPTION,
	EXYNOS7_G3D_OPTION,
	EXYNOS7_MSCL_OPTION,
	EXYNOS7_MFC_OPTION,
	EXYNOS7_BUS0_OPTION,
	EXYNOS7_FSYS0_OPTION,
	EXYNOS7_FSYS1_OPTION,
	EXYNOS7_ISP0_OPTION,
	EXYNOS7_ISP1_OPTION,
	EXYNOS7_VPP_OPTION,
};

static void exynos7_set_wakeupmask(enum sys_powerdown mode)
{
	u32 intmask = 0;

	pmu_raw_writel(exynos_get_eint_wake_mask(), EXYNOS7_EINT_WAKEUP_MASK);

	switch (mode) {
	case SYS_SLEEP:
		/* BIT(31): deactivate wakeup event monitoring circuit */
		intmask = 0x7FFFFFFF;
		break;
	default:
		break;
	}
	pmu_raw_writel(intmask, EXYNOS7_WAKEUP_MASK);
	pmu_raw_writel(0xFFFF0000, EXYNOS7_WAKEUP_MASK2);
	pmu_raw_writel(0xFFFF0000, EXYNOS7_WAKEUP_MASK3);
}

static void exynos7_pmu_central_seq(bool enable)
{
	unsigned int tmp;

	/* central sequencer */
	tmp = pmu_raw_readl(EXYNOS7_CENTRAL_SEQ_CONFIGURATION);
	if (enable)
		tmp &= ~EXYNOS7_CENTRALSEQ_PWR_CFG;
	else
		tmp |= EXYNOS7_CENTRALSEQ_PWR_CFG;
	pmu_raw_writel(tmp, EXYNOS7_CENTRAL_SEQ_CONFIGURATION);

	/* central sequencer MIF */
	tmp = pmu_raw_readl(EXYNOS7_CENTRAL_SEQ_MIF_CONFIGURATION);
	if (enable)
		tmp &= ~EXYNOS7_CENTRALSEQ_PWR_CFG;
	else
		tmp |= EXYNOS7_CENTRALSEQ_PWR_CFG;
	pmu_raw_writel(tmp, EXYNOS7_CENTRAL_SEQ_MIF_CONFIGURATION);
}

static void exynos7_powerdown_conf(enum sys_powerdown mode)
{
	exynos7_set_wakeupmask(mode);
	exynos7_pmu_central_seq(true);
	if (!(pmu_raw_readl(EXYNOS7_PMU_DEBUG) &
				EXYNOS7_CLKOUT_DISABLE))
		pmu_raw_writel(0x1, EXYNOS7_XXTI_SYS_PWR_REG);
}

static void exynos7_pmu_init(void)
{
	unsigned int cpu;
	unsigned int tmp, i;
	struct device_node *node;
	static void __iomem *atlas_cmu_base;

	 /* Enable only SC_FEEDBACK for the register list */
	for (i = 0 ; i < ARRAY_SIZE(exynos7_list_feed) ; i++) {
		tmp = pmu_raw_readl(exynos7_list_feed[i]);
		tmp &= ~EXYNOS5_USE_SC_COUNTER;
		tmp |= EXYNOS5_USE_SC_FEEDBACK;
		pmu_raw_writel(tmp, exynos7_list_feed[i]);
	}

	/*
	 * Disable automatic L2 flush, Disable L2 retention and
	 * Enable STANDBYWFIL2, ACE/ACP
	 */
	tmp = pmu_raw_readl(EXYNOS7_ATLAS_L2_OPTION);
	tmp &= ~(EXYNOS7_USE_AUTO_L2FLUSHREQ | EXYNOS7_USE_RETENTION);
	tmp |= (EXYNOS7_USE_STANDBYWFIL2 |
		EXYNOS7_USE_DEACTIVATE_ACE |
		EXYNOS7_USE_DEACTIVATE_ACP);
	pmu_raw_writel(tmp, EXYNOS7_ATLAS_L2_OPTION);

	/*
	 * Enable both SC_COUNTER and SC_FEEDBACK for the CPUs
	 * Use STANDBYWFI and SMPEN to indicate that core is ready to enter
	 * low power mode
	 */
	for (cpu = 0; cpu < 4; cpu++) {
		tmp = pmu_raw_readl(EXYNOS7_CPU_OPTION(cpu));
		tmp |= (EXYNOS5_USE_SC_FEEDBACK | EXYNOS5_USE_SC_COUNTER);
		tmp |= EXYNOS7_USE_SMPEN;
		tmp |= EXYNOS7_USE_STANDBYWFI;
		tmp &= ~EXYNOS7_USE_STANDBYWFE;
		pmu_raw_writel(tmp, EXYNOS7_CPU_OPTION(cpu));

		tmp = pmu_raw_readl(EXYNOS7_CPU_DURATION(cpu));
		tmp |= EXYNOS7_DUR_WAIT_RESET;
		tmp &= ~EXYNOS7_DUR_SCALL;
		tmp |= EXYNOS7_DUR_SCALL_VALUE;
		pmu_raw_writel(tmp, EXYNOS7_CPU_DURATION(cpu));
	}

	/* Skip atlas block power-off during automatic power down sequence */
	tmp = pmu_raw_readl(EXYNOS7_ATLAS_CPUSEQUENCER_OPTION);
	tmp |= EXYNOS7_SKIP_BLK_PWR_DOWN;
	pmu_raw_writel(tmp, EXYNOS7_ATLAS_CPUSEQUENCER_OPTION);

	/* Limit in-rush current during local power up of cores */
	tmp = pmu_raw_readl(EXYNOS7_UP_SCHEDULER);
	tmp |= EXYNOS7_ENABLE_ATLAS_CPU;
	pmu_raw_writel(tmp, EXYNOS7_UP_SCHEDULER);

	/* Enable PS hold and hardware tripping */
	tmp = pmu_raw_readl(EXYNOS7_PS_HOLD_CONTROL);
	tmp |= EXYNOS7_PS_HOLD_OUTPUT;
	tmp |= EXYNOS7_ENABLE_HW_TRIP;
	pmu_raw_writel(tmp, EXYNOS7_PS_HOLD_CONTROL);

	/* Enable debug area of atlas cpu */
	tmp = pmu_raw_readl(EXYNOS7_ATLAS_DBG_CONFIGURATION);
	tmp |= EXYNOS7_DBG_INITIATE_WAKEUP;
	pmu_raw_writel(tmp, EXYNOS7_ATLAS_DBG_CONFIGURATION);

	/*
	 * Set clock freeze cycle count to 0 before and after arm clamp or
	 * reset signal transition
	 */
	node = of_find_compatible_node(NULL, NULL,
				"samsung,exynos7-clock-atlas");
	if (node) {
		atlas_cmu_base = of_iomap(node, 0);
		if (!atlas_cmu_base)
			return;

		__raw_writel(0x0,
				atlas_cmu_base + EXYNOS7_CORE_ARMCLK_STOPCTRL);
		iounmap(atlas_cmu_base);
	}

	pr_info("Exynos7 PMU has been initialized\n");
}

const struct exynos_pmu_data exynos7_pmu_data = {
	.pmu_config		= exynos7_pmu_config,
	.pmu_init		= exynos7_pmu_init,
	.pmu_config_extra	= exynos7_pmu_config_extra,
	.powerdown_conf		= exynos7_powerdown_conf,
};
