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


/* Register Offset of Xenon SDHC self-defined register */
#define SDHCI_SYS_CFG_INFO			0x0104
#define SDHCI_SLOT_TYPE_SDIO_SHIFT		24
#define SDHCI_NR_SUPPORTED_SLOT_MASK		0x7

#define SDHCI_SYS_OP_CTRL			0x0108
#define SDHCI_AUTO_CLKGATE_DISABLE_MASK		BIT(20)
#define SDHCI_SDCLK_IDLEOFF_ENABLE_SHIFT	8
#define SDHCI_SLOT_ENABLE_SHIFT			0

#define SDHCI_SYS_EXT_OP_CTRL			0x010C

#define SDHCI_SLOT_EMMC_CTRL			0x0130
#define SDHCI_EMMC_VCCQ_MASK			0x3
#define SDHCI_EMMC_VCCQ_1_8V			0x1
#define SDHCI_EMMC_VCCQ_3_3V			0x3

#define SDHCI_SLOT_RETUNING_REQ_CTRL		0x0144
/* retuning compatible */
#define SDHCI_RETUNING_COMPATIBLE		0x1

/* Tuning Parameter */
#define SDHCI_TMR_RETUN_NO_PRESENT		0xF
#define SDHCI_DEF_TUNING_COUNT			0x9

#define SDHCI_DEFAULT_SDCLK_FREQ		(400000)

/* Xenon specific Mode Select value */
#define SDHCI_XENON_CTRL_HS200			0x5
#define SDHCI_XENON_CTRL_HS400			0x6

/* Indicate Card Type is not clear yet */
#define SDHCI_CARD_TYPE_UNKNOWN			0xF

struct sdhci_xenon_priv {
	unsigned char	tuning_count;
	/* idx of SDHC */
	u8		sdhc_id;

	/*
	 * eMMC/SD/SDIO require different PHY settings or
	 * voltage control. It's necessary for Xenon driver to
	 * recognize card type during, or even before initialization.
	 * However, mmc_host->card is not available yet at that time.
	 * This field records the card type during init.
	 * For eMMC, it is updated in dt parse. For SD/SDIO, it is
	 * updated in xenon_init_card().
	 *
	 * It is only valid during initialization after it is updated.
	 * Do not access this variable in normal transfers after
	 * initialization completes.
	 */
	unsigned int	init_card_type;
};

#endif
