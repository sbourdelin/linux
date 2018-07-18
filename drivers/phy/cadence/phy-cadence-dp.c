/*
 * Cadence MHDP DisplayPort SD0801 PHY driver.
 *
 * Copyright 2018 Cadence Design Systems, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/phy/phy.h>
#include "phy-cadence-dp.h"


static const struct phy_ops cdns_dp_phy_ops = {
	.init		= cdns_dp_phy_init,
	.owner		= THIS_MODULE,
};

static const struct of_device_id cdns_dp_phy_of_match[] = {
	{
		.compatible = "cdns,dp-phy"
	},
	{}
};

MODULE_DEVICE_TABLE(of, cdns_dp_phy_of_match);

static int cdns_dp_phy_probe(struct platform_device *pdev)
{
	struct resource *regs;
	struct cdns_dp_phy *cdns_phy;
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	struct phy *phy;
	int err;

	cdns_phy = devm_kzalloc(dev, sizeof(*cdns_phy), GFP_KERNEL);
	if (!cdns_phy)
		return -ENOMEM;

	cdns_phy->dev = &pdev->dev;

	phy = devm_phy_create(dev, NULL, &cdns_dp_phy_ops);
	if (IS_ERR(phy)) {
		dev_err(dev, "failed to create DisplayPort PHY\n");
		return PTR_ERR(phy);
	}

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	cdns_phy->base = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(cdns_phy->base))
		return PTR_ERR(cdns_phy->base);

	regs = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	cdns_phy->sd_base = devm_ioremap_resource(&pdev->dev, regs);
	if (IS_ERR(cdns_phy->sd_base))
		return PTR_ERR(cdns_phy->sd_base);

	err = device_property_read_u32(dev, "num_lanes",
				       &(cdns_phy->num_lanes));
	if (err)
		cdns_phy->num_lanes = DEFAULT_NUM_LANES;

	switch (cdns_phy->num_lanes) {
	case 1:
	case 2:
	case 4:
		/* valid number of lanes */
		break;
	default:
		dev_err(dev, "unsupported number of lanes: %d\n",
			cdns_phy->num_lanes);
		return -EINVAL;
	}

	err = device_property_read_u32(dev, "max_bit_rate",
		   &(cdns_phy->max_bit_rate));
	if (err)
		cdns_phy->max_bit_rate = DEFAULT_MAX_BIT_RATE;

	switch (cdns_phy->max_bit_rate) {
	case 2160:
	case 2430:
	case 2700:
	case 3240:
	case 4320:
	case 5400:
	case 8100:
		/* valid bit rate */
		break;
	default:
		dev_err(dev, "unsupported max bit rate: %dMbps\n",
			cdns_phy->max_bit_rate);
		return -EINVAL;
	}

	phy_set_drvdata(phy, cdns_phy);

	phy_provider = devm_of_phy_provider_register(dev, of_phy_simple_xlate);

	dev_info(dev, "%d lanes, max bit rate %d.%03d Gbps\n",
		 cdns_phy->num_lanes,
		 cdns_phy->max_bit_rate / 1000,
		 cdns_phy->max_bit_rate % 1000);

	return PTR_ERR_OR_ZERO(phy_provider);
}


static int cdns_dp_phy_init(struct phy *phy)
{
	unsigned char lane_bits;

	struct cdns_dp_phy *cdns_phy = phy_get_drvdata(phy);

	writel(0x0003, cdns_phy->base + PHY_AUX_CTRL); /* enable AUX */

	/* PHY PMA registers configuration function */
	cdns_dp_phy_pma_cfg(cdns_phy);

	/* Set lines power state to A0
	 * Set lines pll clk enable to 0
	 */

	cdns_dp_phy_write_field(cdns_phy, PHY_PMA_XCVR_POWER_STATE_REQ,
				PHY_POWER_STATE_LN_0, 6, 0x0000);

	if (cdns_phy->num_lanes >= 2) {
		cdns_dp_phy_write_field(cdns_phy,
					PHY_PMA_XCVR_POWER_STATE_REQ,
					PHY_POWER_STATE_LN_1, 6, 0x0000);

		if (cdns_phy->num_lanes == 4) {
			cdns_dp_phy_write_field(cdns_phy,
						PHY_PMA_XCVR_POWER_STATE_REQ,
						PHY_POWER_STATE_LN_2, 6, 0);
			cdns_dp_phy_write_field(cdns_phy,
						PHY_PMA_XCVR_POWER_STATE_REQ,
						PHY_POWER_STATE_LN_3, 6, 0);
		}
	}

	cdns_dp_phy_write_field(cdns_phy, PHY_PMA_XCVR_PLLCLK_EN,
				0, 1, 0x0000);

	if (cdns_phy->num_lanes >= 2) {
		cdns_dp_phy_write_field(cdns_phy, PHY_PMA_XCVR_PLLCLK_EN,
					1, 1, 0x0000);
		if (cdns_phy->num_lanes == 4) {
			cdns_dp_phy_write_field(cdns_phy,
						PHY_PMA_XCVR_PLLCLK_EN,
						2, 1, 0x0000);
			cdns_dp_phy_write_field(cdns_phy,
						PHY_PMA_XCVR_PLLCLK_EN,
						3, 1, 0x0000);
		}
	}

	/* release phy_l0*_reset_n and pma_tx_elec_idle_ln_* based on
	 * used lanes
	 */
	lane_bits = (1 << cdns_phy->num_lanes) - 1;
	writel(((0xF & ~lane_bits) << 4) | (0xF & lane_bits),
		   cdns_phy->base + PHY_RESET);

	/* release pma_xcvr_pllclk_en_ln_*, only for the master lane */
	writel(0x0001, cdns_phy->base + PHY_PMA_XCVR_PLLCLK_EN);

	/* PHY PMA registers configuration functions */
	cdns_dp_phy_pma_cmn_vco_cfg_25mhz(cdns_phy);
	cdns_dp_phy_pma_cmn_rate(cdns_phy);

	/* take out of reset */
	cdns_dp_phy_write_field(cdns_phy, PHY_RESET, 8, 1, 1);
	cdns_dp_phy_wait_pma_cmn_ready(cdns_phy);
	cdns_dp_phy_run(cdns_phy);

	return 0;
}

static void cdns_dp_phy_wait_pma_cmn_ready(struct cdns_dp_phy *cdns_phy)
{
	unsigned int reg;
	int ret;

	ret = readl_poll_timeout(cdns_phy->base + PHY_PMA_CMN_READY, reg,
				 reg & 1, 0, 500);
	if (ret == -ETIMEDOUT)
		dev_err(cdns_phy->dev,
			"timeout waiting for PMA common ready\n");
}

static void cdns_dp_phy_pma_cfg(struct cdns_dp_phy *cdns_phy)
{
	unsigned int i;

	/* PMA common configuration */
	cdns_dp_phy_pma_cmn_cfg_25mhz(cdns_phy);

	/* PMA lane configuration to deal with multi-link operation */
	for (i = 0; i < cdns_phy->num_lanes; i++)
		cdns_dp_phy_pma_lane_cfg(cdns_phy, i);
}

static void cdns_dp_phy_pma_cmn_cfg_25mhz(struct cdns_dp_phy *cdns_phy)
{
	/* refclock registers - assumes 25 MHz refclock */
	writel(0x0019, cdns_phy->sd_base + CMN_SSM_BIAS_TMR);
	writel(0x0032, cdns_phy->sd_base + CMN_PLLSM0_PLLPRE_TMR);
	writel(0x00D1, cdns_phy->sd_base + CMN_PLLSM0_PLLLOCK_TMR);
	writel(0x0032, cdns_phy->sd_base + CMN_PLLSM1_PLLPRE_TMR);
	writel(0x00D1, cdns_phy->sd_base + CMN_PLLSM1_PLLLOCK_TMR);
	writel(0x007D, cdns_phy->sd_base + CMN_BGCAL_INIT_TMR);
	writel(0x007D, cdns_phy->sd_base + CMN_BGCAL_ITER_TMR);
	writel(0x0019, cdns_phy->sd_base + CMN_IBCAL_INIT_TMR);
	writel(0x001E, cdns_phy->sd_base + CMN_TXPUCAL_INIT_TMR);
	writel(0x0006, cdns_phy->sd_base + CMN_TXPUCAL_ITER_TMR);
	writel(0x001E, cdns_phy->sd_base + CMN_TXPDCAL_INIT_TMR);
	writel(0x0006, cdns_phy->sd_base + CMN_TXPDCAL_ITER_TMR);
	writel(0x02EE, cdns_phy->sd_base + CMN_RXCAL_INIT_TMR);
	writel(0x0006, cdns_phy->sd_base + CMN_RXCAL_ITER_TMR);
	writel(0x0002, cdns_phy->sd_base + CMN_SD_CAL_INIT_TMR);
	writel(0x0002, cdns_phy->sd_base + CMN_SD_CAL_ITER_TMR);
	writel(0x000E, cdns_phy->sd_base + CMN_SD_CAL_REFTIM_START);
	writel(0x012B, cdns_phy->sd_base + CMN_SD_CAL_PLLCNT_START);
	/* PLL registers */
	writel(0x0409, cdns_phy->sd_base + CMN_PDIAG_PLL0_CP_PADJ_M0);
	writel(0x1001, cdns_phy->sd_base + CMN_PDIAG_PLL0_CP_IADJ_M0);
	writel(0x0F08, cdns_phy->sd_base + CMN_PDIAG_PLL0_FILT_PADJ_M0);
	writel(0x0004, cdns_phy->sd_base + CMN_PLL0_DSM_DIAG_M0);
	writel(0x00FA, cdns_phy->sd_base + CMN_PLL0_VCOCAL_INIT_TMR);
	writel(0x0004, cdns_phy->sd_base + CMN_PLL0_VCOCAL_ITER_TMR);
	writel(0x00FA, cdns_phy->sd_base + CMN_PLL1_VCOCAL_INIT_TMR);
	writel(0x0004, cdns_phy->sd_base + CMN_PLL1_VCOCAL_ITER_TMR);
	writel(0x0318, cdns_phy->sd_base + CMN_PLL0_VCOCAL_REFTIM_START);
}

static void cdns_dp_phy_pma_cmn_vco_cfg_25mhz(struct cdns_dp_phy *cdns_phy)
{
	/* Assumes 25 MHz refclock */
	switch (cdns_phy->max_bit_rate) {
		/* Setting VCO for 10.8GHz */
	case 2700:
	case 5400:
		writel(0x01B0, cdns_phy->sd_base + CMN_PLL0_INTDIV_M0);
		writel(0x0000, cdns_phy->sd_base + CMN_PLL0_FRACDIVL_M0);
		writel(0x0002, cdns_phy->sd_base + CMN_PLL0_FRACDIVH_M0);
		writel(0x0120, cdns_phy->sd_base + CMN_PLL0_HIGH_THR_M0);
		break;
		/* Setting VCO for 9.72GHz */
	case 2430:
	case 3240:
		writel(0x0184, cdns_phy->sd_base + CMN_PLL0_INTDIV_M0);
		writel(0xCCCD, cdns_phy->sd_base + CMN_PLL0_FRACDIVL_M0);
		writel(0x0002, cdns_phy->sd_base + CMN_PLL0_FRACDIVH_M0);
		writel(0x0104, cdns_phy->sd_base + CMN_PLL0_HIGH_THR_M0);
		break;
		/* Setting VCO for 8.64GHz */
	case 2160:
	case 4320:
		writel(0x0159, cdns_phy->sd_base + CMN_PLL0_INTDIV_M0);
		writel(0x999A, cdns_phy->sd_base + CMN_PLL0_FRACDIVL_M0);
		writel(0x0002, cdns_phy->sd_base + CMN_PLL0_FRACDIVH_M0);
		writel(0x00E7, cdns_phy->sd_base + CMN_PLL0_HIGH_THR_M0);
		break;
		/* Setting VCO for 8.1GHz */
	case 8100:
		writel(0x0144, cdns_phy->sd_base + CMN_PLL0_INTDIV_M0);
		writel(0x0000, cdns_phy->sd_base + CMN_PLL0_FRACDIVL_M0);
		writel(0x0002, cdns_phy->sd_base + CMN_PLL0_FRACDIVH_M0);
		writel(0x00D8, cdns_phy->sd_base + CMN_PLL0_HIGH_THR_M0);
		break;
	}

	writel(0x0002, cdns_phy->sd_base + CMN_PDIAG_PLL0_CTRL_M0);
	writel(0x0318, cdns_phy->sd_base + CMN_PLL0_VCOCAL_PLLCNT_START);
}

static void cdns_dp_phy_pma_cmn_rate(struct cdns_dp_phy *cdns_phy)
{
	unsigned int clk_sel_val = 0;
	unsigned int hsclk_div_val = 0;
	unsigned int i;

	/* 16'h0000 for single DP link configuration */
	writel(0x0000, cdns_phy->sd_base + PHY_PLL_CFG);

	switch (cdns_phy->max_bit_rate) {
	case 1620:
		clk_sel_val = 0x0f01;
		hsclk_div_val = 2;
		break;
	case 2160:
	case 2430:
	case 2700:
		clk_sel_val = 0x0701;
		 hsclk_div_val = 1;
		break;
	case 3240:
		clk_sel_val = 0x0b00;
		hsclk_div_val = 2;
		break;
	case 4320:
	case 5400:
		clk_sel_val = 0x0301;
		hsclk_div_val = 0;
		break;
	case 8100:
		clk_sel_val = 0x0200;
		hsclk_div_val = 0;
		break;
	}

	writel(clk_sel_val, cdns_phy->sd_base + CMN_PDIAG_PLL0_CLK_SEL_M0);

	/* PMA lane configuration to deal with multi-link operation */
	for (i = 0; i < cdns_phy->num_lanes; i++) {
		writel(hsclk_div_val,
		       cdns_phy->sd_base + (XCVR_DIAG_HSCLK_DIV | (i<<11)));
	}
}

static void cdns_dp_phy_pma_lane_cfg(struct cdns_dp_phy *cdns_phy,
				     unsigned int lane)
{
	unsigned int i;

	i = 0x0007 & lane;
	/* Writing Tx/Rx Power State Controllers registers */
	writel(0x00FB, cdns_phy->sd_base + (TX_PSC_A0 | (i<<11)));
	writel(0x04AA, cdns_phy->sd_base + (TX_PSC_A2 | (i<<11)));
	writel(0x04AA, cdns_phy->sd_base + (TX_PSC_A3 | (i<<11)));
	writel(0x0000, cdns_phy->sd_base + (RX_PSC_A0 | (i<<11)));
	writel(0x0000, cdns_phy->sd_base + (RX_PSC_A2 | (i<<11)));
	writel(0x0000, cdns_phy->sd_base + (RX_PSC_A3 | (i<<11)));

	writel(0x0001, cdns_phy->sd_base + (XCVR_DIAG_PLLDRC_CTRL | (i<<11)));
	writel(0x0000, cdns_phy->sd_base + (XCVR_DIAG_HSCLK_SEL | (i<<11)));
}

static void cdns_dp_phy_run(struct cdns_dp_phy *cdns_phy)
{
	unsigned int read_val;
	u32 write_val1 = 0;
	u32 write_val2 = 0;
	u32 mask = 0;
	int ret;

	/* waiting for ACK of pma_xcvr_pllclk_en_ln_*, only for the
	 * master lane
	 */
	ret = readl_poll_timeout(cdns_phy->base + PHY_PMA_XCVR_PLLCLK_EN_ACK,
				 read_val, read_val & 1, 0, POLL_TIMEOUT_US);
	if (ret == -ETIMEDOUT)
		dev_err(cdns_phy->dev,
			"timeout waiting for link PLL clock enable ack\n");

	ndelay(100);

	switch (cdns_phy->num_lanes) {

	case 1:	/* lane 0 */
		write_val1 = 0x00000004;
		write_val2 = 0x00000001;
		mask = 0x0000003f;
		break;
	case 2: /* lane 0-1 */
		write_val1 = 0x00000404;
		write_val2 = 0x00000101;
		mask = 0x00003f3f;
		break;
	case 4: /* lane 0-3 */
		write_val1 = 0x04040404;
		write_val2 = 0x01010101;
		mask = 0x3f3f3f3f;
		break;
	}

	writel(write_val1, cdns_phy->base + PHY_PMA_XCVR_POWER_STATE_REQ);

	ret = readl_poll_timeout(cdns_phy->base + PHY_PMA_XCVR_POWER_STATE_ACK,
				 read_val, (read_val & mask) == write_val1, 0,
				 POLL_TIMEOUT_US);
	if (ret == -ETIMEDOUT)
		dev_err(cdns_phy->dev,
			"timeout waiting for link power state ack\n");

	writel(0, cdns_phy->base + PHY_PMA_XCVR_POWER_STATE_REQ);
	ndelay(100);

	writel(write_val2, cdns_phy->base + PHY_PMA_XCVR_POWER_STATE_REQ);

	ret = readl_poll_timeout(cdns_phy->base + PHY_PMA_XCVR_POWER_STATE_ACK,
				 read_val, (read_val & mask) == write_val2, 0,
				 POLL_TIMEOUT_US);
	if (ret == -ETIMEDOUT)
		dev_err(cdns_phy->dev,
			"timeout waiting for link power state ack\n");

	writel(0, cdns_phy->base + PHY_PMA_XCVR_POWER_STATE_REQ);
	ndelay(100);
}

static void cdns_dp_phy_write_field(struct cdns_dp_phy *cdns_phy,
				    unsigned int offset,
				    unsigned char start_bit,
				    unsigned char num_bits,
				    unsigned int val)
{
	unsigned int read_val;

	read_val = readl(cdns_phy->base + offset);
	writel(((val << start_bit) | (read_val & ~(((1 << num_bits) - 1) <<
		start_bit))), cdns_phy->base + offset);
}

static struct platform_driver cdns_dp_phy_driver = {
	.probe	= cdns_dp_phy_probe,
	.driver = {
		.name	= "cdns-dp-phy",
		.of_match_table	= cdns_dp_phy_of_match,
	}
};
module_platform_driver(cdns_dp_phy_driver);

MODULE_AUTHOR("Scott Telford <stelford@cadence.com>");
MODULE_DESCRIPTION("Cadence MHDP PHY driver");
MODULE_LICENSE("GPL v2");
