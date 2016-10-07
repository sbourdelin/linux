/*
 * PHY support for Xenon SDHC
 *
 * Copyright (C) 2016 Marvell, All Rights Reserved.
 *
 * Author:	Hu Ziji <huziji@marvell.com>
 * Date:	2016-8-24
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/of_address.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/card.h>
#include <linux/mmc/sdio.h>

#include "sdhci.h"
#include "sdhci-pltfm.h"
#include "sdhci-xenon.h"

static const char * const phy_types[] = {
	"sdh phy",
	"emmc 5.0 phy",
	"emmc 5.1 phy"
};

enum phy_type_enum {
	SDH_PHY,
	EMMC_5_0_PHY,
	EMMC_5_1_PHY,
	NR_PHY_TYPES
};

struct soc_pad_ctrl_table {
	const char *soc;
	void (*set_soc_pad)(struct sdhci_host *host,
			    unsigned char signal_voltage);
};

struct soc_pad_ctrl {
	/* Register address of SOC PHY PAD ctrl */
	void __iomem	*reg;
	/* SOC PHY PAD ctrl type */
	enum soc_pad_ctrl_type pad_type;
	/* SOC specific operation to set SOC PHY PAD */
	void (*set_soc_pad)(struct sdhci_host *host,
			    unsigned char signal_voltage);
};

static struct xenon_emmc_phy_regs  xenon_emmc_5_0_phy_regs = {
	.timing_adj	= EMMC_5_0_PHY_TIMING_ADJUST,
	.func_ctrl	= EMMC_5_0_PHY_FUNC_CONTROL,
	.pad_ctrl	= EMMC_5_0_PHY_PAD_CONTROL,
	.pad_ctrl2	= EMMC_5_0_PHY_PAD_CONTROL2,
	.dll_ctrl	= EMMC_5_0_PHY_DLL_CONTROL,
	.logic_timing_adj = EMMC_5_0_PHY_LOGIC_TIMING_ADJUST,
	.delay_mask	= EMMC_5_0_PHY_FIXED_DELAY_MASK,
	.dll_update	= DLL_UPDATE_STROBE_5_0,
};

static struct xenon_emmc_phy_regs  xenon_emmc_5_1_phy_regs = {
	.timing_adj	= EMMC_PHY_TIMING_ADJUST,
	.func_ctrl	= EMMC_PHY_FUNC_CONTROL,
	.pad_ctrl	= EMMC_PHY_PAD_CONTROL,
	.pad_ctrl2	= EMMC_PHY_PAD_CONTROL2,
	.dll_ctrl	= EMMC_PHY_DLL_CONTROL,
	.logic_timing_adj = EMMC_PHY_LOGIC_TIMING_ADJUST,
	.delay_mask	= EMMC_PHY_FIXED_DELAY_MASK,
	.dll_update	= DLL_UPDATE,
};

static int xenon_delay_adj_test(struct mmc_card *card);

/*
 * eMMC PHY configuration and operations
 */
struct emmc_phy_params {
	bool	slow_mode;

	u8	znr;
	u8	zpr;

	/* Nr of consecutive Sampling Points of a Valid Sampling Window */
	u8	nr_tun_times;
	/* Divider for calculating Tuning Step */
	u8	tun_step_divider;

	struct soc_pad_ctrl pad_ctrl;
};

static void xenon_emmc_phy_strobe_delay_adj(struct sdhci_host *host,
					    struct mmc_card *card);
static int xenon_emmc_phy_fix_sampl_delay_adj(struct sdhci_host *host,
					      struct mmc_card *card);
static void xenon_emmc_phy_set(struct sdhci_host *host,
			       unsigned char timing);
static void xenon_emmc_set_soc_pad(struct sdhci_host *host,
				   unsigned char signal_voltage);

static const struct xenon_phy_ops emmc_phy_ops = {
	.strobe_delay_adj = xenon_emmc_phy_strobe_delay_adj,
	.fix_sampl_delay_adj = xenon_emmc_phy_fix_sampl_delay_adj,
	.phy_set = xenon_emmc_phy_set,
	.set_soc_pad = xenon_emmc_set_soc_pad,
};

static int alloc_emmc_phy(struct sdhci_xenon_priv *priv)
{
	struct emmc_phy_params *params;

	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!params)
		return -ENOMEM;

	priv->phy_params = params;
	priv->phy_ops = &emmc_phy_ops;
	if (priv->phy_type == EMMC_5_0_PHY)
		priv->emmc_phy_regs = &xenon_emmc_5_0_phy_regs;
	else
		priv->emmc_phy_regs = &xenon_emmc_5_1_phy_regs;

	return 0;
}

static int xenon_emmc_phy_init(struct sdhci_host *host)
{
	u32 reg;
	u32 wait, clock;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	struct xenon_emmc_phy_regs *phy_regs = priv->emmc_phy_regs;

	reg = sdhci_readl(host, phy_regs->timing_adj);
	reg |= PHY_INITIALIZAION;
	sdhci_writel(host, reg, phy_regs->timing_adj);

	/* Add duration of FC_SYNC_RST */
	wait = ((reg >> FC_SYNC_RST_DURATION_SHIFT) &
			FC_SYNC_RST_DURATION_MASK);
	/* Add interval between FC_SYNC_EN and FC_SYNC_RST */
	wait += ((reg >> FC_SYNC_RST_EN_DURATION_SHIFT) &
			FC_SYNC_RST_EN_DURATION_MASK);
	/* Add duration of asserting FC_SYNC_EN */
	wait += ((reg >> FC_SYNC_EN_DURATION_SHIFT) &
			FC_SYNC_EN_DURATION_MASK);
	/* Add duration of waiting for PHY */
	wait += ((reg >> WAIT_CYCLE_BEFORE_USING_SHIFT) &
			WAIT_CYCLE_BEFORE_USING_MASK);
	/* 4 addtional bus clock and 4 AXI bus clock are required */
	wait += 8;
	wait <<= 20;

	clock = host->clock;
	if (!clock)
		/* Use the possibly slowest bus frequency value */
		clock = LOWEST_SDCLK_FREQ;
	/* get the wait time */
	wait /= clock;
	wait++;
	/* wait for host eMMC PHY init completes */
	udelay(wait);

	reg = sdhci_readl(host, phy_regs->timing_adj);
	reg &= PHY_INITIALIZAION;
	if (reg) {
		dev_err(mmc_dev(host->mmc), "eMMC PHY init cannot complete after %d us\n",
			wait);
		return -ETIMEDOUT;
	}

	return 0;
}

#define ARMADA_3700_SOC_PAD_1_8V	0x1
#define ARMADA_3700_SOC_PAD_3_3V	0x0

static void armada_3700_soc_pad_voltage_set(struct sdhci_host *host,
					    unsigned char signal_voltage)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	struct emmc_phy_params *params = priv->phy_params;

	if (params->pad_ctrl.pad_type == SOC_PAD_FIXED_1_8V) {
		writel(ARMADA_3700_SOC_PAD_1_8V, params->pad_ctrl.reg);
	} else if (params->pad_ctrl.pad_type == SOC_PAD_SD) {
		if (signal_voltage == MMC_SIGNAL_VOLTAGE_180)
			writel(ARMADA_3700_SOC_PAD_1_8V, params->pad_ctrl.reg);
		else if (signal_voltage == MMC_SIGNAL_VOLTAGE_330)
			writel(ARMADA_3700_SOC_PAD_3_3V, params->pad_ctrl.reg);
	}
}

static void xenon_emmc_set_soc_pad(struct sdhci_host *host,
				   unsigned char signal_voltage)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	struct emmc_phy_params *params = priv->phy_params;

	if (!params->pad_ctrl.reg)
		return;

	if (params->pad_ctrl.set_soc_pad)
		params->pad_ctrl.set_soc_pad(host, signal_voltage);
}

static int emmc_phy_set_fix_sampl_delay(struct sdhci_host *host,
					unsigned int delay,
					bool invert,
					bool delay_90_degree)
{
	u32 reg;
	unsigned long flags;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	struct xenon_emmc_phy_regs *phy_regs = priv->emmc_phy_regs;
	int ret = 0;

	spin_lock_irqsave(&host->lock, flags);

	/* Setup Sampling fix delay */
	reg = sdhci_readl(host, SDHC_SLOT_OP_STATUS_CTRL);
	reg &= ~phy_regs->delay_mask;
	reg |= delay & phy_regs->delay_mask;
	sdhci_writel(host, reg, SDHC_SLOT_OP_STATUS_CTRL);

	if (priv->phy_type == EMMC_5_0_PHY) {
		/* set 90 degree phase if necessary */
		reg &= ~DELAY_90_DEGREE_MASK_EMMC5;
		reg |= (delay_90_degree << DELAY_90_DEGREE_SHIFT_EMMC5);
		sdhci_writel(host, reg, SDHC_SLOT_OP_STATUS_CTRL);
	}

	/* Disable SDCLK */
	reg = sdhci_readl(host, SDHCI_CLOCK_CONTROL);
	reg &= ~(SDHCI_CLOCK_CARD_EN | SDHCI_CLOCK_INT_EN);
	sdhci_writel(host, reg, SDHCI_CLOCK_CONTROL);

	udelay(200);

	if (priv->phy_type == EMMC_5_1_PHY) {
		/* set 90 degree phase if necessary */
		reg = sdhci_readl(host, EMMC_PHY_FUNC_CONTROL);
		reg &= ~ASYNC_DDRMODE_MASK;
		reg |= (delay_90_degree << ASYNC_DDRMODE_SHIFT);
		sdhci_writel(host, reg, EMMC_PHY_FUNC_CONTROL);
	}

	/* Setup Inversion of Sampling edge */
	reg = sdhci_readl(host, phy_regs->timing_adj);
	reg &= ~SAMPL_INV_QSP_PHASE_SELECT;
	reg |= (invert << SAMPL_INV_QSP_PHASE_SELECT_SHIFT);
	sdhci_writel(host, reg, phy_regs->timing_adj);

	/* Enable SD internal clock */
	ret = enable_xenon_internal_clk(host);
	if (ret)
		goto out;

	/* Enable SDCLK */
	reg = sdhci_readl(host, SDHCI_CLOCK_CONTROL);
	reg |= SDHCI_CLOCK_CARD_EN;
	sdhci_writel(host, reg, SDHCI_CLOCK_CONTROL);

	udelay(200);

	/*
	 * Has to re-initialize eMMC PHY here to active PHY
	 * because later get status cmd will be issued.
	 */
	ret = xenon_emmc_phy_init(host);

out:
	spin_unlock_irqrestore(&host->lock, flags);
	return ret;
}

static int emmc_phy_do_fix_sampl_delay(struct sdhci_host *host,
				       struct mmc_card *card,
				       unsigned int delay,
				       bool invert, bool quarter)
{
	int ret;

	emmc_phy_set_fix_sampl_delay(host, delay, invert, quarter);

	ret = xenon_delay_adj_test(card);
	if (ret) {
		dev_dbg(mmc_dev(host->mmc),
			"fail when sampling fix delay = %d, phase = %d degree\n",
			delay, invert * 180 + quarter * 90);
		return -1;
	}
	return 0;
}

static int xenon_emmc_phy_fix_sampl_delay_adj(struct sdhci_host *host,
					      struct mmc_card *card)
{
	enum sampl_fix_delay_phase phase;
	int idx, nr_pair;
	int ret;
	unsigned int delay;
	unsigned int min_delay, max_delay;
	bool invert, quarter;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	struct xenon_emmc_phy_regs *phy_regs = priv->emmc_phy_regs;
	u32 coarse_step, fine_step;
	const enum sampl_fix_delay_phase delay_edge[] = {
		PHASE_0_DEGREE,
		PHASE_180_DEGREE,
		PHASE_90_DEGREE,
		PHASE_270_DEGREE
	};

	coarse_step = phy_regs->delay_mask >> 1;
	fine_step = coarse_step >> 2;

	nr_pair = ARRAY_SIZE(delay_edge);

	for (idx = 0; idx < nr_pair; idx++) {
		phase = delay_edge[idx];
		invert = (phase & 0x2) ? true : false;
		quarter = (phase & 0x1) ? true : false;

		/* increase delay value to get fix delay */
		for (min_delay = 0;
		     min_delay <= phy_regs->delay_mask;
		     min_delay += coarse_step) {
			ret = emmc_phy_do_fix_sampl_delay(host, card, min_delay,
							  invert, quarter);
			if (!ret)
				break;
		}

		if (ret) {
			dev_dbg(mmc_dev(host->mmc),
				"Fail to set Sampling Fixed Delay with phase = %d degree\n",
				phase * 90);
			continue;
		}

		for (max_delay = min_delay + fine_step;
		     max_delay < phy_regs->delay_mask;
		     max_delay += fine_step) {
			ret = emmc_phy_do_fix_sampl_delay(host, card, max_delay,
							  invert, quarter);
			if (ret) {
				max_delay -= fine_step;
				break;
			}
		}

		if (!ret) {
			ret = emmc_phy_do_fix_sampl_delay(host, card,
							  phy_regs->delay_mask,
							  invert, quarter);
			if (!ret)
				max_delay = phy_regs->delay_mask;
		}

		/*
		 * Sampling Fixed Delay line window should be large enough,
		 * thus the sampling point (the middle of the window)
		 * can work when environment varies.
		 * However, there is no clear conclusion how large the window
		 * should be.
		 */
		if ((max_delay - min_delay) <=
		    EMMC_PHY_FIXED_DELAY_WINDOW_MIN) {
			dev_info(mmc_dev(host->mmc),
				 "The window size %d with phase = %d degree is too small\n",
				 max_delay - min_delay, phase * 90);
			continue;
		}

		delay = (min_delay + max_delay) / 2;
		emmc_phy_set_fix_sampl_delay(host, delay, invert, quarter);
		dev_dbg(mmc_dev(host->mmc),
			"sampling fix delay = %d with phase = %d degree\n",
			delay, phase * 90);
		return 0;
	}

	return -EIO;
}

static int xenon_emmc_phy_enable_dll(struct sdhci_host *host)
{
	u32 reg;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	struct xenon_emmc_phy_regs *phy_regs = priv->emmc_phy_regs;
	u8 timeout;

	if (WARN_ON(host->clock <= MMC_HIGH_52_MAX_DTR))
		return -EINVAL;

	reg = sdhci_readl(host, phy_regs->dll_ctrl);
	if (reg & DLL_ENABLE)
		return 0;

	/* Enable DLL */
	reg = sdhci_readl(host, phy_regs->dll_ctrl);
	reg |= (DLL_ENABLE | DLL_FAST_LOCK);

	/*
	 * Set Phase as 90 degree, which is most common value.
	 * Might set another value if necessary.
	 * The granularity is 1 degree.
	 */
	reg &= ~((DLL_PHASE_MASK << DLL_PHSEL0_SHIFT) |
			(DLL_PHASE_MASK << DLL_PHSEL1_SHIFT));
	reg |= ((DLL_PHASE_90_DEGREE << DLL_PHSEL0_SHIFT) |
			(DLL_PHASE_90_DEGREE << DLL_PHSEL1_SHIFT));

	reg &= ~DLL_BYPASS_EN;
	reg |= phy_regs->dll_update;
	if (priv->phy_type == EMMC_5_1_PHY)
		reg &= ~DLL_REFCLK_SEL;
	sdhci_writel(host, reg, phy_regs->dll_ctrl);

	/* Wait max 32 ms */
	timeout = 32;
	while (!(sdhci_readw(host, SDHC_SLOT_EXT_PRESENT_STATE) & LOCK_STATE)) {
		if (!timeout) {
			dev_err(mmc_dev(host->mmc), "Wait for DLL Lock time-out\n");
			return -ETIMEDOUT;
		}
		timeout--;
		mdelay(1);
	}
	return 0;
}

static int __emmc_phy_config_tuning(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	struct emmc_phy_params *params = priv->phy_params;
	u32 reg, tuning_step;
	int ret;
	unsigned long flags;

	if (WARN_ON(host->clock <= MMC_HIGH_52_MAX_DTR))
		return -EINVAL;

	spin_lock_irqsave(&host->lock, flags);

	ret = xenon_emmc_phy_enable_dll(host);
	if (ret) {
		spin_unlock_irqrestore(&host->lock, flags);
		return ret;
	}

	reg = sdhci_readl(host, SDHC_SLOT_DLL_CUR_DLY_VAL);
	tuning_step = reg / params->tun_step_divider;
	if (unlikely(tuning_step > TUNING_STEP_MASK)) {
		dev_warn(mmc_dev(host->mmc),
			 "HS200 TUNING_STEP %d is larger than MAX value\n",
			 tuning_step);
		tuning_step = TUNING_STEP_MASK;
	}

	reg = sdhci_readl(host, SDHC_SLOT_OP_STATUS_CTRL);
	reg &= ~(TUN_CONSECUTIVE_TIMES_MASK << TUN_CONSECUTIVE_TIMES_SHIFT);
	reg |= (params->nr_tun_times << TUN_CONSECUTIVE_TIMES_SHIFT);
	reg &= ~(TUNING_STEP_MASK << TUNING_STEP_SHIFT);
	reg |= (tuning_step << TUNING_STEP_SHIFT);
	sdhci_writel(host, reg, SDHC_SLOT_OP_STATUS_CTRL);

	spin_unlock_irqrestore(&host->lock, flags);
	return 0;
}

static int xenon_emmc_phy_config_tuning(struct sdhci_host *host)
{
	return __emmc_phy_config_tuning(host);
}

static void xenon_emmc_phy_strobe_delay_adj(struct sdhci_host *host,
					    struct mmc_card *card)
{
	u32 reg;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	unsigned long flags;

	if (host->clock <= MMC_HIGH_52_MAX_DTR)
		return;

	dev_dbg(mmc_dev(host->mmc), "starts HS400 strobe delay adjustment\n");

	spin_lock_irqsave(&host->lock, flags);

	xenon_emmc_phy_enable_dll(host);

	/* Enable SDHC Data Strobe */
	reg = sdhci_readl(host, SDHC_SLOT_EMMC_CTRL);
	reg |= ENABLE_DATA_STROBE;
	sdhci_writel(host, reg, SDHC_SLOT_EMMC_CTRL);

	/* Set Data Strobe Pull down */
	if (priv->phy_type == EMMC_5_0_PHY) {
		reg = sdhci_readl(host, EMMC_5_0_PHY_PAD_CONTROL);
		reg |= EMMC5_FC_QSP_PD;
		reg &= ~EMMC5_FC_QSP_PU;
		sdhci_writel(host, reg, EMMC_5_0_PHY_PAD_CONTROL);
	} else {
		reg = sdhci_readl(host, EMMC_PHY_PAD_CONTROL1);
		reg |= EMMC5_1_FC_QSP_PD;
		reg &= ~EMMC5_1_FC_QSP_PU;
		sdhci_writel(host, reg, EMMC_PHY_PAD_CONTROL1);
	}
	spin_unlock_irqrestore(&host->lock, flags);
}

#define LOGIC_TIMING_VALUE	0x00AA8977

static void xenon_emmc_phy_set(struct sdhci_host *host,
			       unsigned char timing)
{
	u32 reg;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	struct emmc_phy_params *params = priv->phy_params;
	struct xenon_emmc_phy_regs *phy_regs = priv->emmc_phy_regs;
	struct mmc_card *card = priv->card_candidate;
	unsigned long flags;

	dev_dbg(mmc_dev(host->mmc), "eMMC PHY setting starts\n");

	spin_lock_irqsave(&host->lock, flags);

	/* Setup pad, set bit[28] and bits[26:24] */
	reg = sdhci_readl(host, phy_regs->pad_ctrl);
	reg |= (FC_DQ_RECEN | FC_CMD_RECEN | FC_QSP_RECEN | OEN_QSN);
	/*
	 * All FC_XX_RECEIVCE should be set as CMOS Type
	 */
	reg |= FC_ALL_CMOS_RECEIVER;
	sdhci_writel(host, reg, phy_regs->pad_ctrl);

	/* Set CMD and DQ Pull Up */
	if (priv->phy_type == EMMC_5_0_PHY) {
		reg = sdhci_readl(host, EMMC_5_0_PHY_PAD_CONTROL);
		reg |= (EMMC5_FC_CMD_PU | EMMC5_FC_DQ_PU);
		reg &= ~(EMMC5_FC_CMD_PD | EMMC5_FC_DQ_PD);
		sdhci_writel(host, reg, EMMC_5_0_PHY_PAD_CONTROL);
	} else {
		reg = sdhci_readl(host, EMMC_PHY_PAD_CONTROL1);
		reg |= (EMMC5_1_FC_CMD_PU | EMMC5_1_FC_DQ_PU);
		reg &= ~(EMMC5_1_FC_CMD_PD | EMMC5_1_FC_DQ_PD);
		sdhci_writel(host, reg, EMMC_PHY_PAD_CONTROL1);
	}

	if ((timing == MMC_TIMING_LEGACY) || !card)
		goto phy_init;

	/*
	 * FIXME: should depends on the specific board timing.
	 */
	if ((timing == MMC_TIMING_MMC_HS400) ||
	    (timing == MMC_TIMING_MMC_HS200) ||
	    (timing == MMC_TIMING_UHS_SDR50) ||
	    (timing == MMC_TIMING_UHS_SDR104) ||
	    (timing == MMC_TIMING_UHS_DDR50) ||
	    (timing == MMC_TIMING_UHS_SDR25) ||
	    (timing == MMC_TIMING_MMC_DDR52)) {
		reg = sdhci_readl(host, phy_regs->timing_adj);
		reg &= ~OUTPUT_QSN_PHASE_SELECT;
		sdhci_writel(host, reg, phy_regs->timing_adj);
	}

	/*
	 * If SDIO card, set SDIO Mode
	 * Otherwise, clear SDIO Mode and Slow Mode
	 */
	if (mmc_card_sdio(card)) {
		reg = sdhci_readl(host, phy_regs->timing_adj);
		reg |= TIMING_ADJUST_SDIO_MODE;

		if ((timing == MMC_TIMING_UHS_SDR25) ||
		    (timing == MMC_TIMING_UHS_SDR12) ||
		    (timing == MMC_TIMING_SD_HS) ||
		    (timing == MMC_TIMING_LEGACY))
			reg |= TIMING_ADJUST_SLOW_MODE;

		sdhci_writel(host, reg, phy_regs->timing_adj);
	} else {
		reg = sdhci_readl(host, phy_regs->timing_adj);
		reg &= ~(TIMING_ADJUST_SDIO_MODE | TIMING_ADJUST_SLOW_MODE);
		sdhci_writel(host, reg, phy_regs->timing_adj);
	}

	if (((timing == MMC_TIMING_UHS_SDR50) ||
	     (timing == MMC_TIMING_UHS_SDR25) ||
	     (timing == MMC_TIMING_UHS_SDR12) ||
	     (timing == MMC_TIMING_SD_HS) ||
	     (timing == MMC_TIMING_MMC_HS) ||
	     (timing == MMC_TIMING_LEGACY)) && params->slow_mode) {
		reg = sdhci_readl(host, phy_regs->timing_adj);
		reg |= TIMING_ADJUST_SLOW_MODE;
		sdhci_writel(host, reg, phy_regs->timing_adj);
	}

	/*
	 * Set preferred ZNR and ZPR value
	 * The ZNR and ZPR value vary between different boards.
	 * Define them both in sdhci-xenon-emmc-phy.h.
	 */
	reg = sdhci_readl(host, phy_regs->pad_ctrl2);
	reg &= ~((ZNR_MASK << ZNR_SHIFT) | ZPR_MASK);
	reg |= ((params->znr << ZNR_SHIFT) | params->zpr);
	sdhci_writel(host, reg, phy_regs->pad_ctrl2);

	/*
	 * When setting EMMC_PHY_FUNC_CONTROL register,
	 * SD clock should be disabled
	 */
	reg = sdhci_readl(host, SDHCI_CLOCK_CONTROL);
	reg &= ~SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, reg, SDHCI_CLOCK_CONTROL);

	if ((timing == MMC_TIMING_UHS_DDR50) ||
	    (timing == MMC_TIMING_MMC_HS400) ||
	    (timing == MMC_TIMING_MMC_DDR52)) {
		reg = sdhci_readl(host, phy_regs->func_ctrl);
		reg |= (DQ_DDR_MODE_MASK << DQ_DDR_MODE_SHIFT) | CMD_DDR_MODE;
		sdhci_writel(host, reg, phy_regs->func_ctrl);
	}

	if (timing == MMC_TIMING_MMC_HS400) {
		reg = sdhci_readl(host, phy_regs->func_ctrl);
		reg &= ~DQ_ASYNC_MODE;
		sdhci_writel(host, reg, phy_regs->func_ctrl);
	}

	/* Enable bus clock */
	reg = sdhci_readl(host, SDHCI_CLOCK_CONTROL);
	reg |= SDHCI_CLOCK_CARD_EN;
	sdhci_writew(host, reg, SDHCI_CLOCK_CONTROL);

	if (timing == MMC_TIMING_MMC_HS400)
		/* Hardware team recommend a value for HS400 */
		sdhci_writel(host, LOGIC_TIMING_VALUE,
			     phy_regs->logic_timing_adj);

phy_init:
	xenon_emmc_phy_init(host);

	spin_unlock_irqrestore(&host->lock, flags);

	dev_dbg(mmc_dev(host->mmc), "eMMC PHY setting completes\n");
}

static int get_dt_pad_ctrl_data(struct sdhci_host *host,
				struct device_node *np,
				struct emmc_phy_params *params)
{
	int ret = 0;
	const char *name;
	struct resource iomem;

	if (of_device_is_compatible(np, "marvell,armada-3700-sdhci"))
		params->pad_ctrl.set_soc_pad = armada_3700_soc_pad_voltage_set;
	else
		return 0;

	if (of_address_to_resource(np, 1, &iomem)) {
		dev_err(mmc_dev(host->mmc), "Unable to find SOC PAD ctrl register address for %s\n",
			np->name);
		return -EINVAL;
	}

	params->pad_ctrl.reg = devm_ioremap_resource(mmc_dev(host->mmc),
						     &iomem);
	if (IS_ERR(params->pad_ctrl.reg)) {
		dev_err(mmc_dev(host->mmc), "Unable to get SOC PHY PAD ctrl regiser for %s\n",
			np->name);
		return PTR_ERR(params->pad_ctrl.reg);
	}

	ret = of_property_read_string(np, "xenon,pad-type", &name);
	if (ret) {
		dev_err(mmc_dev(host->mmc), "Unable to determine SOC PHY PAD ctrl type\n");
		return ret;
	}
	if (!strcmp(name, "sd")) {
		params->pad_ctrl.pad_type = SOC_PAD_SD;
	} else if (!strcmp(name, "fixed-1-8v")) {
		params->pad_ctrl.pad_type = SOC_PAD_FIXED_1_8V;
	} else {
		dev_err(mmc_dev(host->mmc), "Unsupported SOC PHY PAD ctrl type %s\n",
			name);
		return -EINVAL;
	}

	return ret;
}

static int emmc_phy_parse_param_dt(struct sdhci_host *host,
				   struct device_node *np,
				   struct emmc_phy_params *params)
{
	u32 value;

	if (of_property_read_bool(np, "xenon,phy-slow-mode"))
		params->slow_mode = true;
	else
		params->slow_mode = false;

	if (!of_property_read_u32(np, "xenon,phy-znr", &value))
		params->znr = value & ZNR_MASK;
	else
		params->znr = ZNR_DEF_VALUE;

	if (!of_property_read_u32(np, "xenon,phy-zpr", &value))
		params->zpr = value & ZPR_MASK;
	else
		params->zpr = ZPR_DEF_VALUE;

	if (!of_property_read_u32(np, "xenon,phy-nr-tun-times", &value))
		params->nr_tun_times = value & TUN_CONSECUTIVE_TIMES_MASK;
	else
		params->nr_tun_times = TUN_CONSECUTIVE_TIMES;

	if (!of_property_read_u32(np, "xenon,phy-tun-step-divider", &value))
		params->tun_step_divider = value & 0xFF;
	else
		params->tun_step_divider = TUNING_STEP_DIVIDER;

	return get_dt_pad_ctrl_data(host, np, params);
}

/*
 * SDH PHY configuration and operations
 */
static int xenon_sdh_phy_set_fix_sampl_delay(struct sdhci_host *host,
					     unsigned int delay, bool invert)
{
	u32 reg;
	unsigned long flags;
	int ret;

	if (invert)
		invert = 0x1;
	else
		invert = 0x0;

	spin_lock_irqsave(&host->lock, flags);

	/* Disable SDCLK */
	reg = sdhci_readl(host, SDHCI_CLOCK_CONTROL);
	reg &= ~(SDHCI_CLOCK_CARD_EN | SDHCI_CLOCK_INT_EN);
	sdhci_writel(host, reg, SDHCI_CLOCK_CONTROL);

	udelay(200);

	/* Setup Sampling fix delay */
	reg = sdhci_readl(host, SDHC_SLOT_OP_STATUS_CTRL);
	reg &= ~(SDH_PHY_FIXED_DELAY_MASK |
			(0x1 << FORCE_SEL_INVERSE_CLK_SHIFT));
	reg |= ((delay & SDH_PHY_FIXED_DELAY_MASK) |
			(invert << FORCE_SEL_INVERSE_CLK_SHIFT));
	sdhci_writel(host, reg, SDHC_SLOT_OP_STATUS_CTRL);

	/* Enable SD internal clock */
	ret = enable_xenon_internal_clk(host);

	/* Enable SDCLK */
	reg = sdhci_readl(host, SDHCI_CLOCK_CONTROL);
	reg |= SDHCI_CLOCK_CARD_EN;
	sdhci_writel(host, reg, SDHCI_CLOCK_CONTROL);

	udelay(200);

	spin_unlock_irqrestore(&host->lock, flags);
	return ret;
}

static int sdh_phy_do_fix_sampl_delay(struct sdhci_host *host,
				      struct mmc_card *card,
				      unsigned int delay, bool invert)
{
	int ret;

	xenon_sdh_phy_set_fix_sampl_delay(host, delay, invert);

	ret = xenon_delay_adj_test(card);
	if (ret) {
		dev_dbg(mmc_dev(host->mmc),
			"fail when sampling fix delay = %d, phase = %d degree\n",
			delay, invert * 180);
		return -1;
	}
	return 0;
}

#define SDH_PHY_COARSE_FIX_DELAY	(SDH_PHY_FIXED_DELAY_MASK / 2)
#define SDH_PHY_FINE_FIX_DELAY		(SDH_PHY_COARSE_FIX_DELAY / 4)

static int xenon_sdh_phy_fix_sampl_delay_adj(struct sdhci_host *host,
					     struct mmc_card *card)
{
	u32 reg;
	bool dll_enable = false;
	unsigned int min_delay, max_delay, delay;
	const bool sampl_edge[] = {
		false,
		true,
	};
	int i, nr;
	int ret;

	if (host->clock > HIGH_SPEED_MAX_DTR) {
		/* Enable DLL when SDCLK is higher than 50MHz */
		reg = sdhci_readl(host, SDH_PHY_SLOT_DLL_CTRL);
		if (!(reg & SDH_PHY_ENABLE_DLL)) {
			reg |= (SDH_PHY_ENABLE_DLL | SDH_PHY_FAST_LOCK_EN);
			sdhci_writel(host, reg, SDH_PHY_SLOT_DLL_CTRL);
			mdelay(1);

			reg = sdhci_readl(host, SDH_PHY_SLOT_DLL_PHASE_SEL);
			reg |= SDH_PHY_DLL_UPDATE_TUNING;
			sdhci_writel(host, reg, SDH_PHY_SLOT_DLL_PHASE_SEL);
		}
		dll_enable = true;
	}

	nr = dll_enable ? ARRAY_SIZE(sampl_edge) : 1;
	for (i = 0; i < nr; i++) {
		for (min_delay = 0; min_delay <= SDH_PHY_FIXED_DELAY_MASK;
				min_delay += SDH_PHY_COARSE_FIX_DELAY) {
			ret = sdh_phy_do_fix_sampl_delay(host, card, min_delay,
							 sampl_edge[i]);
			if (!ret)
				break;
		}

		if (ret) {
			dev_dbg(mmc_dev(host->mmc),
				"Fail to set Fixed Sampling Delay with %s edge\n",
				sampl_edge[i] ? "negative" : "positive");
			continue;
		}

		for (max_delay = min_delay + SDH_PHY_FINE_FIX_DELAY;
				max_delay < SDH_PHY_FIXED_DELAY_MASK;
				max_delay += SDH_PHY_FINE_FIX_DELAY) {
			ret = sdh_phy_do_fix_sampl_delay(host, card, max_delay,
							 sampl_edge[i]);
			if (ret) {
				max_delay -= SDH_PHY_FINE_FIX_DELAY;
				break;
			}
		}

		if (!ret) {
			delay = SDH_PHY_FIXED_DELAY_MASK;
			ret = sdh_phy_do_fix_sampl_delay(host, card, delay,
							 sampl_edge[i]);
			if (!ret)
				max_delay = SDH_PHY_FIXED_DELAY_MASK;
		}

		if ((max_delay - min_delay) <= SDH_PHY_FIXED_DELAY_WINDOW_MIN) {
			dev_info(mmc_dev(host->mmc),
				 "The window size %d with %s edge is too small\n",
				 max_delay - min_delay,
				 sampl_edge[i] ? "negative" : "positive");
			continue;
		}

		delay = (min_delay + max_delay) / 2;
		xenon_sdh_phy_set_fix_sampl_delay(host, delay, sampl_edge[i]);
		dev_dbg(mmc_dev(host->mmc), "sampling fix delay = %d with %s edge\n",
			delay, sampl_edge[i] ? "negative" : "positive");
		return 0;
	}
	return -EIO;
}

static const struct xenon_phy_ops sdh_phy_ops = {
	.fix_sampl_delay_adj = xenon_sdh_phy_fix_sampl_delay_adj,
};

static int alloc_sdh_phy(struct sdhci_xenon_priv *priv)
{
	priv->phy_params = NULL;
	priv->phy_ops = &sdh_phy_ops;
	return 0;
}

/*
 * Common functions for all PHYs
 */
void xenon_soc_pad_ctrl(struct sdhci_host *host,
			unsigned char signal_voltage)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);

	if (priv->phy_ops->set_soc_pad)
		priv->phy_ops->set_soc_pad(host, signal_voltage);
}

static int __xenon_emmc_delay_adj_test(struct mmc_card *card)
{
	int err;
	u8 *ext_csd = NULL;

	err = mmc_get_ext_csd(card, &ext_csd);
	kfree(ext_csd);

	return err;
}

static int __xenon_sdio_delay_adj_test(struct mmc_card *card)
{
	struct mmc_command cmd = {0};
	int err;

	cmd.opcode = SD_IO_RW_DIRECT;
	cmd.flags = MMC_RSP_R5 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	if (err)
		return err;

	if (cmd.resp[0] & R5_ERROR)
		return -EIO;
	if (cmd.resp[0] & R5_FUNCTION_NUMBER)
		return -EINVAL;
	if (cmd.resp[0] & R5_OUT_OF_RANGE)
		return -ERANGE;
	return 0;
}

static int __xenon_sd_delay_adj_test(struct mmc_card *card)
{
	struct mmc_command cmd = {0};
	int err;

	cmd.opcode = MMC_SEND_STATUS;
	cmd.arg = card->rca << 16;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_AC;

	err = mmc_wait_for_cmd(card->host, &cmd, 0);
	return err;
}

static int xenon_delay_adj_test(struct mmc_card *card)
{
	WARN_ON(!card);
	WARN_ON(!card->host);

	if (mmc_card_mmc(card))
		return __xenon_emmc_delay_adj_test(card);
	else if (mmc_card_sd(card))
		return __xenon_sd_delay_adj_test(card);
	else if (mmc_card_sdio(card))
		return __xenon_sdio_delay_adj_test(card);
	else
		return -EINVAL;
}

static void xenon_phy_set(struct sdhci_host *host, unsigned char timing)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);

	if (priv->phy_ops->phy_set)
		priv->phy_ops->phy_set(host, timing);
}

static void xenon_hs400_strobe_delay_adj(struct sdhci_host *host,
					 struct mmc_card *card)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);

	if (WARN_ON(!mmc_card_hs400(card)))
		return;

	/* Enable the DLL to automatically adjust HS400 strobe delay.
	 */
	if (priv->phy_ops->strobe_delay_adj)
		priv->phy_ops->strobe_delay_adj(host, card);
}

static int xenon_fix_sampl_delay_adj(struct sdhci_host *host,
				     struct mmc_card *card)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);

	if (priv->phy_ops->fix_sampl_delay_adj)
		return priv->phy_ops->fix_sampl_delay_adj(host, card);

	return 0;
}

/*
 * xenon_delay_adj should not be called inside IRQ context,
 * either Hard IRQ or Softirq.
 */
static int xenon_hs_delay_adj(struct sdhci_host *host,
			      struct mmc_card *card)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	int ret = 0;

	if (WARN_ON(host->clock <= DEFAULT_SDCLK_FREQ))
		return -EINVAL;

	if (mmc_card_hs400(card)) {
		xenon_hs400_strobe_delay_adj(host, card);
		return 0;
	}

	if (((priv->phy_type == EMMC_5_1_PHY) ||
	     (priv->phy_type == EMMC_5_0_PHY)) &&
	     (mmc_card_hs200(card) ||
	     (host->timing == MMC_TIMING_UHS_SDR104))) {
		ret = xenon_emmc_phy_config_tuning(host);
		if (!ret)
			return 0;
	}

	ret = xenon_fix_sampl_delay_adj(host, card);
	if (ret)
		dev_err(mmc_dev(host->mmc), "fails sampling fixed delay adjustment\n");
	return ret;
}

int xenon_phy_adj(struct sdhci_host *host, struct mmc_ios *ios)
{
	struct mmc_host *mmc = host->mmc;
	struct mmc_card *card;
	int ret = 0;
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);

	if (!host->clock) {
		priv->clock = 0;
		return 0;
	}

	/*
	 * The timing, frequency or bus width is changed,
	 * better to set eMMC PHY based on current setting
	 * and adjust Xenon SDHC delay.
	 */
	if ((host->clock == priv->clock) &&
	    (ios->bus_width == priv->bus_width) &&
	    (ios->timing == priv->timing))
		return 0;

	xenon_phy_set(host, ios->timing);

	/* Update the record */
	priv->bus_width = ios->bus_width;
	/* Temp stage from HS200 to HS400 */
	if (((priv->timing == MMC_TIMING_MMC_HS200) &&
	     (ios->timing == MMC_TIMING_MMC_HS)) ||
	    ((ios->timing == MMC_TIMING_MMC_HS) &&
	     (priv->clock > host->clock))) {
		priv->timing = ios->timing;
		priv->clock = host->clock;
		return 0;
	}
	priv->timing = ios->timing;
	priv->clock = host->clock;

	/* Legacy mode is a special case */
	if (ios->timing == MMC_TIMING_LEGACY)
		return 0;

	card = priv->card_candidate;
	if (unlikely(!card)) {
		dev_warn(mmc_dev(mmc), "card is not present\n");
		return -EINVAL;
	}

	if (host->clock > DEFAULT_SDCLK_FREQ)
		ret = xenon_hs_delay_adj(host, card);
	return ret;
}

static int add_xenon_phy(struct device_node *np, struct sdhci_host *host,
			 const char *phy_name)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_xenon_priv *priv = sdhci_pltfm_priv(pltfm_host);
	int i, ret;

	for (i = 0; i < NR_PHY_TYPES; i++) {
		if (!strcmp(phy_name, phy_types[i])) {
			priv->phy_type = i;
			break;
		}
	}
	if (i == NR_PHY_TYPES) {
		dev_err(mmc_dev(host->mmc),
			"Unable to determine PHY name %s. Use default eMMC 5.1 PHY\n",
			phy_name);
		priv->phy_type = EMMC_5_1_PHY;
	}

	if (priv->phy_type == SDH_PHY) {
		return alloc_sdh_phy(priv);
	} else if ((priv->phy_type == EMMC_5_0_PHY) ||
			(priv->phy_type == EMMC_5_1_PHY)) {
		ret = alloc_emmc_phy(priv);
		if (ret)
			return ret;
		return emmc_phy_parse_param_dt(host, np, priv->phy_params);
	}

	return -EINVAL;
}

int xenon_phy_parse_dt(struct device_node *np, struct sdhci_host *host)
{
	const char *phy_type = NULL;

	if (!of_property_read_string(np, "xenon,phy-type", &phy_type))
		return add_xenon_phy(np, host, phy_type);

	dev_err(mmc_dev(host->mmc), "Fail to get Xenon PHY type. Use default eMMC 5.1 PHY\n");
	return add_xenon_phy(np, host, "emmc 5.1 phy");
}
