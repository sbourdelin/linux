/* linux/drivers/mmc/host/sdhci-xenon-phy.h
 *
 * Author:	Hu Ziji <huziji@marvell.com>
 * Date:	2016-8-24
 *
 *  Copyright (C) 2016 Marvell, All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */
#ifndef SDHCI_XENON_PHY_H_
#define SDHCI_XENON_PHY_H_

#include <linux/types.h>
#include "sdhci.h"

/* Register base for eMMC PHY 5.0 Version */
#define EMMC_5_0_PHY_REG_BASE			0x0160
/* Register base for eMMC PHY 5.1 Version */
#define EMMC_PHY_REG_BASE			0x0170

#define EMMC_PHY_TIMING_ADJUST			EMMC_PHY_REG_BASE
#define EMMC_5_0_PHY_TIMING_ADJUST		EMMC_5_0_PHY_REG_BASE
#define TIMING_ADJUST_SLOW_MODE			BIT(29)
#define TIMING_ADJUST_SDIO_MODE			BIT(28)
#define OUTPUT_QSN_PHASE_SELECT			BIT(17)
#define SAMPL_INV_QSP_PHASE_SELECT		BIT(18)
#define SAMPL_INV_QSP_PHASE_SELECT_SHIFT	18
#define PHY_INITIALIZAION			BIT(31)
#define WAIT_CYCLE_BEFORE_USING_MASK		0xF
#define WAIT_CYCLE_BEFORE_USING_SHIFT		12
#define FC_SYNC_EN_DURATION_MASK		0xF
#define FC_SYNC_EN_DURATION_SHIFT		8
#define FC_SYNC_RST_EN_DURATION_MASK		0xF
#define FC_SYNC_RST_EN_DURATION_SHIFT		4
#define FC_SYNC_RST_DURATION_MASK		0xF
#define FC_SYNC_RST_DURATION_SHIFT		0

#define EMMC_PHY_FUNC_CONTROL			(EMMC_PHY_REG_BASE + 0x4)
#define EMMC_5_0_PHY_FUNC_CONTROL		(EMMC_5_0_PHY_REG_BASE + 0x4)
#define ASYNC_DDRMODE_MASK			BIT(23)
#define ASYNC_DDRMODE_SHIFT			23
#define CMD_DDR_MODE				BIT(16)
#define DQ_DDR_MODE_SHIFT			8
#define DQ_DDR_MODE_MASK			0xFF
#define DQ_ASYNC_MODE				BIT(4)

#define EMMC_PHY_PAD_CONTROL			(EMMC_PHY_REG_BASE + 0x8)
#define EMMC_5_0_PHY_PAD_CONTROL		(EMMC_5_0_PHY_REG_BASE + 0x8)
#define REC_EN_SHIFT				24
#define REC_EN_MASK				0xF
#define FC_DQ_RECEN				BIT(24)
#define FC_CMD_RECEN				BIT(25)
#define FC_QSP_RECEN				BIT(26)
#define FC_QSN_RECEN				BIT(27)
#define OEN_QSN					BIT(28)
#define AUTO_RECEN_CTRL				BIT(30)
#define FC_ALL_CMOS_RECEIVER			0xF000

#define EMMC5_FC_QSP_PD				BIT(18)
#define EMMC5_FC_QSP_PU				BIT(22)
#define EMMC5_FC_CMD_PD				BIT(17)
#define EMMC5_FC_CMD_PU				BIT(21)
#define EMMC5_FC_DQ_PD				BIT(16)
#define EMMC5_FC_DQ_PU				BIT(20)

#define EMMC_PHY_PAD_CONTROL1			(EMMC_PHY_REG_BASE + 0xC)
#define EMMC5_1_FC_QSP_PD			BIT(9)
#define EMMC5_1_FC_QSP_PU			BIT(25)
#define EMMC5_1_FC_CMD_PD			BIT(8)
#define EMMC5_1_FC_CMD_PU			BIT(24)
#define EMMC5_1_FC_DQ_PD			0xFF
#define EMMC5_1_FC_DQ_PU			(0xFF << 16)

#define EMMC_PHY_PAD_CONTROL2			(EMMC_PHY_REG_BASE + 0x10)
#define EMMC_5_0_PHY_PAD_CONTROL2		(EMMC_5_0_PHY_REG_BASE + 0xC)
#define ZNR_MASK				0x1F
#define ZNR_SHIFT				8
#define ZPR_MASK				0x1F
/* Perferred ZNR and ZPR value vary between different boards.
 * The specific ZNR and ZPR value should be defined here
 * according to board actual timing.
 */
#define ZNR_DEF_VALUE				0xF
#define ZPR_DEF_VALUE				0xF

#define EMMC_PHY_DLL_CONTROL			(EMMC_PHY_REG_BASE + 0x14)
#define EMMC_5_0_PHY_DLL_CONTROL		(EMMC_5_0_PHY_REG_BASE + 0x10)
#define DLL_ENABLE				BIT(31)
#define DLL_UPDATE_STROBE_5_0			BIT(30)
#define DLL_REFCLK_SEL				BIT(30)
#define DLL_UPDATE				BIT(23)
#define DLL_PHSEL1_SHIFT			24
#define DLL_PHSEL0_SHIFT			16
#define DLL_PHASE_MASK				0x3F
#define DLL_PHASE_90_DEGREE			0x1F
#define DLL_FAST_LOCK				BIT(5)
#define DLL_GAIN2X				BIT(3)
#define DLL_BYPASS_EN				BIT(0)

#define EMMC_5_0_PHY_LOGIC_TIMING_ADJUST	(EMMC_5_0_PHY_REG_BASE + 0x14)
#define EMMC_PHY_LOGIC_TIMING_ADJUST		(EMMC_PHY_REG_BASE + 0x18)

enum sampl_fix_delay_phase {
	PHASE_0_DEGREE = 0x0,
	PHASE_90_DEGREE = 0x1,
	PHASE_180_DEGREE = 0x2,
	PHASE_270_DEGREE = 0x3,
};

#define SDH_PHY_SLOT_DLL_CTRL			(0x0138)
#define SDH_PHY_ENABLE_DLL			BIT(1)
#define SDH_PHY_FAST_LOCK_EN			BIT(5)

#define SDH_PHY_SLOT_DLL_PHASE_SEL		(0x013C)
#define SDH_PHY_DLL_UPDATE_TUNING		BIT(15)

enum soc_pad_ctrl_type {
	SOC_PAD_SD,
	SOC_PAD_FIXED_1_8V,
};

/*
 * List offset of PHY registers and some special register values
 * in eMMC PHY 5.0 or eMMC PHY 5.1
 */
struct xenon_emmc_phy_regs {
	/* Offset of Timing Adjust register */
	u16 timing_adj;
	/* Offset of Func Control register */
	u16 func_ctrl;
	/* Offset of Pad Control register */
	u16 pad_ctrl;
	/* Offset of Pad Control register */
	u16 pad_ctrl2;
	/* Offset of DLL Control register */
	u16 dll_ctrl;
	/* Offset of Logic Timing Adjust register */
	u16 logic_timing_adj;
	/* Max value of eMMC Fixed Sampling Delay */
	u32 delay_mask;
	/* DLL Update Enable bit */
	u32 dll_update;
};

struct xenon_phy_ops {
	void (*strobe_delay_adj)(struct sdhci_host *host,
				 struct mmc_card *card);
	int (*fix_sampl_delay_adj)(struct sdhci_host *host,
				   struct mmc_card *card);
	void (*phy_set)(struct sdhci_host *host, unsigned char timing);
	void (*set_soc_pad)(struct sdhci_host *host,
			    unsigned char signal_voltage);
};
#endif
