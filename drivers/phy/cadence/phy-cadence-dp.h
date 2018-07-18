/*
 * Cadence MHDP DisplayPort SD0801 PHY driver.
 *
 * Copyright 2018 Cadence Design Systems, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef _PHY_CADENCE_DP_H
#define _PHY_CADENCE_DP_H

#include <linux/platform_device.h>

#define DEFAULT_NUM_LANES 2
#define MAX_NUM_LANES 4
#define DEFAULT_MAX_BIT_RATE 8100 /* in Mbps */

#define POLL_TIMEOUT_US 2000

/* register offsets from DPTX PHY register block base (i.e MHDP
 * register base + 0x30a00)
 */
#define PHY_AUX_CONFIG			0x00
#define PHY_AUX_CTRL			0x04
#define PHY_RESET			0x20
#define PHY_PMA_XCVR_PLLCLK_EN		0x24
#define PHY_PMA_XCVR_PLLCLK_EN_ACK	0x28
#define PHY_PMA_XCVR_POWER_STATE_REQ	0x2c
#define PHY_POWER_STATE_LN_0	0x0000
#define PHY_POWER_STATE_LN_1	0x0008
#define PHY_POWER_STATE_LN_2	0x0010
#define PHY_POWER_STATE_LN_3	0x0018
#define PHY_PMA_XCVR_POWER_STATE_ACK	0x30
#define PHY_PMA_CMN_READY		0x34
#define PHY_PMA_XCVR_TX_VMARGIN		0x38
#define PHY_PMA_XCVR_TX_DEEMPH		0x3c

/* register offsets from SD0801 PHY register block base (i.e MHDP
 * register base + 0x500000)
 */
#define CMN_SSM_BANDGAP_TMR		0x00084
#define CMN_SSM_BIAS_TMR		0x00088
#define CMN_PLLSM0_PLLPRE_TMR		0x000a8
#define CMN_PLLSM0_PLLLOCK_TMR		0x000b0
#define CMN_PLLSM1_PLLPRE_TMR		0x000c8
#define CMN_PLLSM1_PLLLOCK_TMR		0x000d0
#define CMN_BGCAL_INIT_TMR		0x00190
#define CMN_BGCAL_ITER_TMR		0x00194
#define CMN_IBCAL_INIT_TMR		0x001d0
#define CMN_PLL0_VCOCAL_INIT_TMR	0x00210
#define CMN_PLL0_VCOCAL_ITER_TMR	0x00214
#define CMN_PLL0_VCOCAL_REFTIM_START	0x00218
#define CMN_PLL0_VCOCAL_PLLCNT_START	0x00220
#define CMN_PLL0_INTDIV_M0		0x00240
#define CMN_PLL0_FRACDIVL_M0		0x00244
#define CMN_PLL0_FRACDIVH_M0		0x00248
#define CMN_PLL0_HIGH_THR_M0		0x0024c
#define CMN_PLL0_DSM_DIAG_M0		0x00250
#define CMN_PLL0_LOCK_PLLCNT_START	0x00278
#define CMN_PLL1_VCOCAL_INIT_TMR	0x00310
#define CMN_PLL1_VCOCAL_ITER_TMR	0x00314
#define CMN_PLL1_DSM_DIAG_M0		0x00350
#define CMN_TXPUCAL_INIT_TMR		0x00410
#define CMN_TXPUCAL_ITER_TMR		0x00414
#define CMN_TXPDCAL_INIT_TMR		0x00430
#define CMN_TXPDCAL_ITER_TMR		0x00434
#define CMN_RXCAL_INIT_TMR		0x00450
#define CMN_RXCAL_ITER_TMR		0x00454
#define CMN_SD_CAL_INIT_TMR		0x00490
#define CMN_SD_CAL_ITER_TMR		0x00494
#define CMN_SD_CAL_REFTIM_START		0x00498
#define CMN_SD_CAL_PLLCNT_START		0x004a0
#define CMN_PDIAG_PLL0_CTRL_M0		0x00680
#define CMN_PDIAG_PLL0_CLK_SEL_M0	0x00684
#define CMN_PDIAG_PLL0_CP_PADJ_M0	0x00690
#define CMN_PDIAG_PLL0_CP_IADJ_M0	0x00694
#define CMN_PDIAG_PLL0_FILT_PADJ_M0	0x00698
#define CMN_PDIAG_PLL0_CP_PADJ_M1	0x006d0
#define CMN_PDIAG_PLL0_CP_IADJ_M1	0x006d4
#define CMN_PDIAG_PLL1_CLK_SEL_M0	0x00704
#define XCVR_DIAG_PLLDRC_CTRL		0x10394
#define XCVR_DIAG_HSCLK_SEL		0x10398
#define XCVR_DIAG_HSCLK_DIV		0x1039c
#define TX_PSC_A0			0x10400
#define TX_PSC_A1			0x10404
#define TX_PSC_A2			0x10408
#define TX_PSC_A3			0x1040c
#define RX_PSC_A0			0x20000
#define RX_PSC_A1			0x20004
#define RX_PSC_A2			0x20008
#define RX_PSC_A3			0x2000c
#define PHY_PLL_CFG			0x30038

struct cdns_dp_phy {
	void __iomem *base;	/* DPTX registers base */
	void __iomem *sd_base; /* SD0801 registers base */
	u32 num_lanes; /* Number of lanes to use */
	u32 max_bit_rate; /* Maximum link bit rate to use (in Mbps) */
	struct device *dev;
};

static int cdns_dp_phy_init(struct phy *phy);
static void cdns_dp_phy_run(struct cdns_dp_phy *cdns_phy);
static void cdns_dp_phy_wait_pma_cmn_ready(struct cdns_dp_phy *cdns_phy);
static void cdns_dp_phy_pma_cfg(struct cdns_dp_phy *cdns_phy);
static void cdns_dp_phy_pma_cmn_cfg_25mhz(struct cdns_dp_phy *cdns_phy);
static void cdns_dp_phy_pma_lane_cfg(struct cdns_dp_phy *cdns_phy,
					 unsigned int lane);
static void cdns_dp_phy_pma_cmn_vco_cfg_25mhz(struct cdns_dp_phy *cdns_phy);
static void cdns_dp_phy_pma_cmn_rate(struct cdns_dp_phy *cdns_phy);
static void cdns_dp_phy_write_field(struct cdns_dp_phy *cdns_phy,
					unsigned int offset,
					unsigned char start_bit,
					unsigned char num_bits,
					unsigned int val);

#endif
