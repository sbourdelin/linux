/*
 * DWMAC5 TSN Core Functions
 * Copyright (C) 2017 Synopsys, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * Author: Jose Abreu <joabreu@synopsys.com>
 */

#include "common.h"
#include "dwmac5_tsn.h"

static int dwmac5_est_write(void __iomem *ioaddr, u32 reg, u32 val, bool gcla)
{
	int timeout_ms = EST_WRITE_TIMEOUT_MS;
	u32 ctrl = 0x0;

	writel(val, ioaddr + MTL_EST_GCL_DATA);

	ctrl |= reg;
	ctrl |= gcla ? 0x0 : MTL_EST_GCRR;
	writel(ctrl, ioaddr + MTL_EST_GCL_CONTROL);

	ctrl |= MTL_EST_SRWO;
	writel(ctrl, ioaddr + MTL_EST_GCL_CONTROL);

	while (--timeout_ms) {
		udelay(1000);
		if (readl(ioaddr + MTL_EST_GCL_CONTROL) & MTL_EST_SRWO)
			continue;
		break;
	}

	if (!timeout_ms)
		return -ETIMEDOUT;
	return 0;
}

static void dwmac5_config_est(struct mac_device_info *hw,
			      struct plat_stmmacenet_data *plat)
{
	struct stmmac_est_cfg *est = &plat->est_cfg;
	void __iomem *ioaddr = hw->pcsr;
	u32 ctrl = 0x0, btr[2];
	struct timespec64 now;
	int i;

	/* Add real time to offset */
	ktime_get_real_ts64(&now);
	btr[0] = (u32)now.tv_nsec + est->btr[0];
	btr[1] = (u32)now.tv_sec + est->btr[1];

	/* Write parameters */
	dwmac5_est_write(ioaddr, MTL_EST_BTR_LOW, btr[0], false);
	dwmac5_est_write(ioaddr, MTL_EST_BTR_HIGH, btr[1], false);
	dwmac5_est_write(ioaddr, MTL_EST_CTR_LOW, est->ctr[0], false);
	dwmac5_est_write(ioaddr, MTL_EST_CTR_HIGH, est->ctr[1], false);
	dwmac5_est_write(ioaddr, MTL_EST_TER, est->ter, false);
	dwmac5_est_write(ioaddr, MTL_EST_LLR, est->llr, false);

	/* Write GCL table */
	for (i = 0; i < est->llr; i++) {
		u32 reg = (i << MTL_EST_ADDR_SHIFT) & MTL_EST_ADDR;
		dwmac5_est_write(ioaddr, reg, est->gcl[i], true);
	}

	/* Enable EST */
	ctrl |= MTL_EST_EEST;
	writel(ctrl, ioaddr + MTL_EST_CONTROL);

	/* Store table */
	ctrl |= MTL_EST_SSWL;
	writel(ctrl, ioaddr + MTL_EST_CONTROL);
}

void dwmac5_config_fp(struct mac_device_info *hw)
{
	void __iomem *ioaddr = hw->pcsr;
	u32 ctrl;

	/* Enable frame preemption */
	ctrl = readl(ioaddr + GMAC_FPE_CTRL_STS);
	ctrl |= GMAC_FPE_EFPE;
	writel(ctrl, ioaddr + GMAC_FPE_CTRL_STS);
}

void dwmac5_config_tsn(struct mac_device_info *hw,
		       struct plat_stmmacenet_data *plat)
{
	void __iomem *ioaddr = hw->pcsr;
	u32 value = readl(ioaddr + GMAC_HW_FEATURE3);

	if ((value & GMAC_HW_FEAT_ESTSEL) && plat->est_en)
		dwmac5_config_est(hw, plat);
	if ((value & GMAC_HW_FEAT_FPESEL) && plat->fp_en)
		dwmac5_config_fp(hw);
}
