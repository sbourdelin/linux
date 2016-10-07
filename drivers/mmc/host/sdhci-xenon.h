/*
 * Copyright (C) 2016 Marvell, All Rights Reserved.
 *
 * Author:	Hu Ziji <huziji@marvell.com>
 * Date:	2016-8-24
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 */
#ifndef SDHCI_XENON_H_
#define SDHCI_XENON_H_

#include <linux/clk.h>
#include <linux/mmc/card.h>
#include <linux/of.h>
#include "sdhci.h"

/* Register Offset of SD Host Controller SOCP self-defined register */
#define SDHC_SYS_CFG_INFO			0x0104
#define SLOT_TYPE_SDIO_SHIFT			24
#define SLOT_TYPE_EMMC_MASK			0xFF
#define SLOT_TYPE_EMMC_SHIFT			16
#define SLOT_TYPE_SD_SDIO_MMC_MASK		0xFF
#define SLOT_TYPE_SD_SDIO_MMC_SHIFT		8
#define NR_SUPPORTED_SLOT_MASK			0x7

#define SDHC_SYS_OP_CTRL			0x0108
#define AUTO_CLKGATE_DISABLE_MASK		BIT(20)
#define SDCLK_IDLEOFF_ENABLE_SHIFT		8
#define SLOT_ENABLE_SHIFT			0

#define SDHC_SYS_EXT_OP_CTRL			0x010C
#define MASK_CMD_CONFLICT_ERROR			BIT(8)

#define SDHC_SLOT_OP_STATUS_CTRL		0x0128
#define DELAY_90_DEGREE_MASK_EMMC5		BIT(7)
#define DELAY_90_DEGREE_SHIFT_EMMC5		7
#define EMMC_5_0_PHY_FIXED_DELAY_MASK		0x7F
#define EMMC_PHY_FIXED_DELAY_MASK		0xFF
#define EMMC_PHY_FIXED_DELAY_WINDOW_MIN		(EMMC_PHY_FIXED_DELAY_MASK >> 3)
#define SDH_PHY_FIXED_DELAY_MASK		0x1FF
#define SDH_PHY_FIXED_DELAY_WINDOW_MIN		(SDH_PHY_FIXED_DELAY_MASK >> 4)

#define TUN_CONSECUTIVE_TIMES_SHIFT		16
#define TUN_CONSECUTIVE_TIMES_MASK		0x7
#define TUN_CONSECUTIVE_TIMES			0x4
#define TUNING_STEP_SHIFT			12
#define TUNING_STEP_MASK			0xF
#define TUNING_STEP_DIVIDER			BIT(6)

#define FORCE_SEL_INVERSE_CLK_SHIFT		11

#define SDHC_SLOT_EMMC_CTRL			0x0130
#define ENABLE_DATA_STROBE			BIT(24)
#define SET_EMMC_RSTN				BIT(16)
#define DISABLE_RD_DATA_CRC			BIT(14)
#define DISABLE_CRC_STAT_TOKEN			BIT(13)
#define EMMC_VCCQ_MASK				0x3
#define EMMC_VCCQ_1_8V				0x1
#define EMMC_VCCQ_3_3V				0x3

#define SDHC_SLOT_RETUNING_REQ_CTRL		0x0144
/* retuning compatible */
#define RETUNING_COMPATIBLE			0x1

#define SDHC_SLOT_EXT_PRESENT_STATE		0x014C
#define LOCK_STATE				0x1

#define SDHC_SLOT_DLL_CUR_DLY_VAL		0x0150

/* Tuning Parameter */
#define TMR_RETUN_NO_PRESENT			0xF
#define DEF_TUNING_COUNT			0x9

#define MMC_TIMING_FAKE				0xFF

#define DEFAULT_SDCLK_FREQ			(400000)

/* Xenon specific Mode Select value */
#define XENON_SDHCI_CTRL_HS200			0x5
#define XENON_SDHCI_CTRL_HS400			0x6

struct sdhci_xenon_priv {
	/*
	 * The bus_width, timing, and clock fields in below
	 * record the current setting of Xenon SDHC.
	 * Driver will call a Sampling Fixed Delay Adjustment
	 * if any setting is changed.
	 */
	unsigned char	bus_width;
	unsigned char	timing;
	unsigned char	tuning_count;
	unsigned int	clock;
	struct clk	*axi_clk;

	/* Slot idx */
	u8		slot_idx;

	/*
	 * When initializing card, Xenon has to determine card type and
	 * adjust Sampling Fixed delay.
	 * However, at that time, card structure is not linked to mmc_host.
	 * Thus a card pointer is added here to provide
	 * the delay adjustment function with the card structure
	 * of the card during initialization
	 */
	struct mmc_card *card_candidate;
};

static inline int enable_xenon_internal_clk(struct sdhci_host *host)
{
	u32 reg;
	u8 timeout;

	reg = sdhci_readl(host, SDHCI_CLOCK_CONTROL);
	reg |= SDHCI_CLOCK_INT_EN;
	sdhci_writel(host, reg, SDHCI_CLOCK_CONTROL);
	/* Wait max 20 ms */
	timeout = 20;
	while (!((reg = sdhci_readw(host, SDHCI_CLOCK_CONTROL))
			& SDHCI_CLOCK_INT_STABLE)) {
		if (timeout == 0) {
			pr_err("%s: Internal clock never stabilised.\n",
			       mmc_hostname(host->mmc));
			return -ETIMEDOUT;
		}
		timeout--;
		mdelay(1);
	}

	return 0;
}
#endif
