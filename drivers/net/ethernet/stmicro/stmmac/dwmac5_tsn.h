/*
 * DWMAC5 TSN Header File
 * Copyright (C) 2017 Synopsys, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * Author: Jose Abreu <joabreu@synopsys.com>
 */

#ifndef __DWMAC5_TSN_H__
#define __DWMAC5_TSN_H__

#include <linux/stmmac.h>

/* MAC registers */
#define GMAC_HW_FEATURE3		0x00000128
#define GMAC_FPE_CTRL_STS		0x00000234

/* MAC HW features3 bitmap */
#define GMAC_HW_FEAT_FPESEL		BIT(26)
#define GMAC_HW_FEAT_ESTSEL		BIT(16)

/* MAC FPE control/status bitmap */
#define GMAC_FPE_EFPE			BIT(0)

/* MTL registers */
#define MTL_EST_CONTROL			0x00000c50
#define MTL_EST_GCL_CONTROL		0x00000c80
#define MTL_EST_GCL_DATA		0x00000c84

/* EST control bitmap */
#define MTL_EST_EEST			BIT(0)
#define MTL_EST_SSWL			BIT(1)

/* EST GCL control bitmap */
#define MTL_EST_ADDR_SHIFT		8
#define MTL_EST_ADDR			GENMASK(19, 8)
#define MTL_EST_GCRR			BIT(2)
#define MTL_EST_SRWO			BIT(0)

/* EST GCRA addresses */
#define MTL_EST_BTR_LOW			(0x0 << MTL_EST_ADDR_SHIFT)
#define MTL_EST_BTR_HIGH		(0x1 << MTL_EST_ADDR_SHIFT)
#define MTL_EST_CTR_LOW			(0x2 << MTL_EST_ADDR_SHIFT)
#define MTL_EST_CTR_HIGH		(0x3 << MTL_EST_ADDR_SHIFT)
#define MTL_EST_TER			(0x4 << MTL_EST_ADDR_SHIFT)
#define MTL_EST_LLR			(0x5 << MTL_EST_ADDR_SHIFT)

/* Misc */
#define EST_WRITE_TIMEOUT_MS		5

void dwmac5_config_tsn(struct mac_device_info *hw,
		       struct plat_stmmacenet_data *plat);

#endif /* __DWMAC5_TSN_H__ */
