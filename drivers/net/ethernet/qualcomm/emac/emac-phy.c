/* Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* Qualcomm Technologies, Inc. EMAC PHY Controller driver.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/pm_runtime.h>
#include <linux/phy.h>
#include "emac.h"
#include "emac-mac.h"
#include "emac-phy.h"
#include "emac-sgmii.h"

/* EMAC base register offsets */
#define EMAC_MDIO_CTRL                                        0x001414
#define EMAC_PHY_STS                                          0x001418
#define EMAC_MDIO_EX_CTRL                                     0x001440

/* EMAC_MDIO_CTRL */
#define MDIO_MODE                                           0x40000000
#define MDIO_PR                                             0x20000000
#define MDIO_AP_EN                                          0x10000000
#define MDIO_BUSY                                            0x8000000
#define MDIO_CLK_SEL_BMSK                                    0x7000000
#define MDIO_CLK_SEL_SHFT                                           24
#define MDIO_START                                            0x800000
#define SUP_PREAMBLE                                          0x400000
#define MDIO_RD_NWR                                           0x200000
#define MDIO_REG_ADDR_BMSK                                    0x1f0000
#define MDIO_REG_ADDR_SHFT                                          16
#define MDIO_DATA_BMSK                                          0xffff
#define MDIO_DATA_SHFT                                               0

/* EMAC_PHY_STS */
#define PHY_ADDR_BMSK                                         0x1f0000
#define PHY_ADDR_SHFT                                               16

/* EMAC_MDIO_EX_CTRL */
#define DEVAD_BMSK                                            0x1f0000
#define DEVAD_SHFT                                                  16
#define EX_REG_ADDR_BMSK                                        0xffff
#define EX_REG_ADDR_SHFT                                             0

#define MDIO_CLK_25_4                                                0
#define MDIO_CLK_25_28                                               7

#define MDIO_WAIT_TIMES                                           1000

/* PHY */
#define MII_PSSR                          0x11 /* PHY Specific Status Reg */

/* MII_BMCR (0x00) */
#define BMCR_SPEED10                    0x0000

/* MII_PSSR (0x11) */
#define PSSR_SPD_DPLX_RESOLVED          0x0800  /* 1=Speed & Duplex resolved */
#define PSSR_DPLX                       0x2000  /* 1=Duplex 0=Half Duplex */
#define PSSR_SPEED                      0xC000  /* Speed, bits 14:15 */
#define PSSR_10MBS                      0x0000  /* 00=10Mbs */
#define PSSR_100MBS                     0x4000  /* 01=100Mbs */
#define PSSR_1000MBS                    0x8000  /* 10=1000Mbs */

#define EMAC_LINK_SPEED_DEFAULT (\
		EMAC_LINK_SPEED_10_HALF  |\
		EMAC_LINK_SPEED_10_FULL  |\
		EMAC_LINK_SPEED_100_HALF |\
		EMAC_LINK_SPEED_100_FULL |\
		EMAC_LINK_SPEED_1GB_FULL)

static int emac_phy_mdio_autopoll_disable(struct emac_adapter *adpt)
{
	int i;
	u32 val;

	emac_reg_update32(adpt->base + EMAC_MDIO_CTRL, MDIO_AP_EN, 0);
	wmb(); /* ensure mdio autopoll disable is requested */

	/* wait for any mdio polling to complete */
	for (i = 0; i < MDIO_WAIT_TIMES; i++) {
		val = readl_relaxed(adpt->base + EMAC_MDIO_CTRL);
		if (!(val & MDIO_BUSY))
			return 0;

		usleep_range(100, 150);
	}

	/* failed to disable; ensure it is enabled before returning */
	emac_reg_update32(adpt->base + EMAC_MDIO_CTRL, 0, MDIO_AP_EN);
	wmb(); /* ensure mdio autopoll is enabled */
	return -EBUSY;
}

static void emac_phy_mdio_autopoll_enable(struct emac_adapter *adpt)
{
	emac_reg_update32(adpt->base + EMAC_MDIO_CTRL, 0, MDIO_AP_EN);
	wmb(); /* ensure mdio autopoll is enabled */
}

int emac_phy_read_reg(struct emac_adapter *adpt, bool ext, u8 dev, bool fast,
		      u16 reg_addr, u16 *phy_data)
{
	struct emac_phy *phy = &adpt->phy;
	u32 clk_sel, val = 0;
	int i;
	int ret = 0;

	*phy_data = 0;
	clk_sel = fast ? MDIO_CLK_25_4 : MDIO_CLK_25_28;

	if (phy->external) {
		ret = emac_phy_mdio_autopoll_disable(adpt);
		if (ret)
			return ret;
	}

	emac_reg_update32(adpt->base + EMAC_PHY_STS, PHY_ADDR_BMSK,
			  (dev << PHY_ADDR_SHFT));
	wmb(); /* ensure PHY address is set before we proceed */

	if (ext) {
		val = ((dev << DEVAD_SHFT) & DEVAD_BMSK) |
		      ((reg_addr << EX_REG_ADDR_SHFT) & EX_REG_ADDR_BMSK);
		writel_relaxed(val, adpt->base + EMAC_MDIO_EX_CTRL);
		wmb(); /* ensure proper address is set before proceeding */

		val = SUP_PREAMBLE |
		      ((clk_sel << MDIO_CLK_SEL_SHFT) & MDIO_CLK_SEL_BMSK) |
		      MDIO_START | MDIO_MODE | MDIO_RD_NWR;
	} else {
		val = val & ~(MDIO_REG_ADDR_BMSK | MDIO_CLK_SEL_BMSK |
				MDIO_MODE | MDIO_PR);
		val = SUP_PREAMBLE |
		      ((clk_sel << MDIO_CLK_SEL_SHFT) & MDIO_CLK_SEL_BMSK) |
		      ((reg_addr << MDIO_REG_ADDR_SHFT) & MDIO_REG_ADDR_BMSK) |
		      MDIO_START | MDIO_RD_NWR;
	}

	writel_relaxed(val, adpt->base + EMAC_MDIO_CTRL);
	mb(); /* ensure hw starts the operation before we check for result */

	for (i = 0; i < MDIO_WAIT_TIMES; i++) {
		val = readl_relaxed(adpt->base + EMAC_MDIO_CTRL);
		if (!(val & (MDIO_START | MDIO_BUSY))) {
			*phy_data = (u16)((val >> MDIO_DATA_SHFT) &
					MDIO_DATA_BMSK);
			break;
		}
		usleep_range(100, 150);
	}

	if (i == MDIO_WAIT_TIMES)
		ret = -EIO;

	if (phy->external)
		emac_phy_mdio_autopoll_enable(adpt);

	return ret;
}

int emac_phy_write_reg(struct emac_adapter *adpt, bool ext, u8 dev, bool fast,
		       u16 reg_addr, u16 phy_data)
{
	struct emac_phy *phy = &adpt->phy;
	u32 clk_sel, val = 0;
	int i;
	int ret = 0;

	clk_sel = fast ? MDIO_CLK_25_4 : MDIO_CLK_25_28;

	if (phy->external) {
		ret = emac_phy_mdio_autopoll_disable(adpt);
		if (ret)
			return ret;
	}

	emac_reg_update32(adpt->base + EMAC_PHY_STS, PHY_ADDR_BMSK,
			  (dev << PHY_ADDR_SHFT));
	wmb(); /* ensure PHY address is set before we proceed */

	if (ext) {
		val = ((dev << DEVAD_SHFT) & DEVAD_BMSK) |
		      ((reg_addr << EX_REG_ADDR_SHFT) & EX_REG_ADDR_BMSK);
		writel_relaxed(val, adpt->base + EMAC_MDIO_EX_CTRL);
		wmb(); /* ensure proper address is set before proceeding */

		val = SUP_PREAMBLE |
			((clk_sel << MDIO_CLK_SEL_SHFT) & MDIO_CLK_SEL_BMSK) |
			((phy_data << MDIO_DATA_SHFT) & MDIO_DATA_BMSK) |
			MDIO_START | MDIO_MODE;
	} else {
		val = val & ~(MDIO_REG_ADDR_BMSK | MDIO_CLK_SEL_BMSK |
			MDIO_DATA_BMSK | MDIO_MODE | MDIO_PR);
		val = SUP_PREAMBLE |
		((clk_sel << MDIO_CLK_SEL_SHFT) & MDIO_CLK_SEL_BMSK) |
		((reg_addr << MDIO_REG_ADDR_SHFT) & MDIO_REG_ADDR_BMSK) |
		((phy_data << MDIO_DATA_SHFT) & MDIO_DATA_BMSK) |
		MDIO_START;
	}

	writel_relaxed(val, adpt->base + EMAC_MDIO_CTRL);
	mb(); /* ensure hw starts the operation before we check for result */

	for (i = 0; i < MDIO_WAIT_TIMES; i++) {
		val = readl_relaxed(adpt->base + EMAC_MDIO_CTRL);
		if (!(val & (MDIO_START | MDIO_BUSY)))
			break;
		usleep_range(100, 150);
	}

	if (i == MDIO_WAIT_TIMES)
		ret = -EIO;

	if (phy->external)
		emac_phy_mdio_autopoll_enable(adpt);

	return ret;
}

int emac_phy_read(struct emac_adapter *adpt, u16 phy_addr, u16 reg_addr,
		  u16 *phy_data)
{
	struct emac_phy *phy = &adpt->phy;
	int  ret;

	mutex_lock(&phy->lock);
	ret = emac_phy_read_reg(adpt, false, phy_addr, true, reg_addr,
				phy_data);
	mutex_unlock(&phy->lock);

	if (ret)
		netdev_err(adpt->netdev, "error: reading phy reg 0x%02x\n",
			   reg_addr);
	else
		netif_dbg(adpt,  hw, adpt->netdev,
			  "EMAC PHY RD: 0x%02x -> 0x%04x\n", reg_addr,
			  *phy_data);

	return ret;
}

int emac_phy_write(struct emac_adapter *adpt, u16 phy_addr, u16 reg_addr,
		   u16 phy_data)
{
	struct emac_phy *phy = &adpt->phy;
	int  ret;

	mutex_lock(&phy->lock);
	ret = emac_phy_write_reg(adpt, false, phy_addr, true, reg_addr,
				 phy_data);
	mutex_unlock(&phy->lock);

	if (ret)
		netdev_err(adpt->netdev, "error: writing phy reg 0x%02x\n",
			   reg_addr);
	else
		netif_dbg(adpt, hw,
			  adpt->netdev, "EMAC PHY WR: 0x%02x <- 0x%04x\n",
			  reg_addr, phy_data);

	return ret;
}

/* initialize external phy */
int emac_phy_external_init(struct emac_adapter *adpt)
{
	struct emac_phy *phy = &adpt->phy;
	u16 phy_id[2];
	int ret = 0;

	if (phy->external) {
		ret = emac_phy_read(adpt, phy->addr, MII_PHYSID1, &phy_id[0]);
		if (ret)
			return ret;

		ret = emac_phy_read(adpt, phy->addr, MII_PHYSID2, &phy_id[1]);
		if (ret)
			return ret;

		phy->id[0] = phy_id[0];
		phy->id[1] = phy_id[1];
	} else {
		emac_phy_mdio_autopoll_disable(adpt);
	}

	return 0;
}

static int emac_phy_link_setup_external(struct emac_adapter *adpt,
					enum emac_flow_ctrl req_fc_mode,
					u32 speed, bool autoneg, bool fc)
{
	struct emac_phy *phy = &adpt->phy;
	u16 adv, bmcr, ctrl1000 = 0;
	int ret = 0;

	if (autoneg) {
		switch (req_fc_mode) {
		case EMAC_FC_FULL:
		case EMAC_FC_RX_PAUSE:
			adv = ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM;
			break;
		case EMAC_FC_TX_PAUSE:
			adv = ADVERTISE_PAUSE_ASYM;
			break;
		default:
			adv = 0;
			break;
		}
		if (!fc)
			adv &= ~(ADVERTISE_PAUSE_CAP | ADVERTISE_PAUSE_ASYM);

		if (speed & EMAC_LINK_SPEED_10_HALF)
			adv |= ADVERTISE_10HALF;

		if (speed & EMAC_LINK_SPEED_10_FULL)
			adv |= ADVERTISE_10HALF | ADVERTISE_10FULL;

		if (speed & EMAC_LINK_SPEED_100_HALF)
			adv |= ADVERTISE_100HALF;

		if (speed & EMAC_LINK_SPEED_100_FULL)
			adv |= ADVERTISE_100HALF | ADVERTISE_100FULL;

		if (speed & EMAC_LINK_SPEED_1GB_FULL)
			ctrl1000 |= ADVERTISE_1000FULL;

		ret |= emac_phy_write(adpt, phy->addr, MII_ADVERTISE, adv);
		ret |= emac_phy_write(adpt, phy->addr, MII_CTRL1000, ctrl1000);

		bmcr = BMCR_RESET | BMCR_ANENABLE | BMCR_ANRESTART;
		ret |= emac_phy_write(adpt, phy->addr, MII_BMCR, bmcr);
	} else {
		bmcr = BMCR_RESET;
		switch (speed) {
		case EMAC_LINK_SPEED_10_HALF:
			bmcr |= BMCR_SPEED10;
			break;
		case EMAC_LINK_SPEED_10_FULL:
			bmcr |= BMCR_SPEED10 | BMCR_FULLDPLX;
			break;
		case EMAC_LINK_SPEED_100_HALF:
			bmcr |= BMCR_SPEED100;
			break;
		case EMAC_LINK_SPEED_100_FULL:
			bmcr |= BMCR_SPEED100 | BMCR_FULLDPLX;
			break;
		default:
			return -EINVAL;
		}

		ret |= emac_phy_write(adpt, phy->addr, MII_BMCR, bmcr);
	}

	return ret;
}

int emac_phy_link_setup(struct emac_adapter *adpt, u32 speed, bool autoneg,
			bool fc)
{
	struct emac_phy *phy = &adpt->phy;
	int ret = 0;

	if (!phy->external)
		return emac_sgmii_no_ephy_link_setup(adpt, speed, autoneg);

	if (emac_phy_link_setup_external(adpt, phy->req_fc_mode, speed, autoneg,
					 fc)) {
		netdev_err(adpt->netdev,
			   "error: on ephy setup speed:%d autoneg:%d fc:%d\n",
			   speed, autoneg, fc);
		ret = -EINVAL;
	} else {
		phy->autoneg = autoneg;
	}

	return ret;
}

int emac_phy_link_check(struct emac_adapter *adpt, u32 *speed, bool *link_up)
{
	struct emac_phy *phy = &adpt->phy;
	u16 bmsr, pssr;
	int ret;

	if (!phy->external) {
		emac_sgmii_no_ephy_link_check(adpt, speed, link_up);
		return 0;
	}

	ret = emac_phy_read(adpt, phy->addr, MII_BMSR, &bmsr);
	if (ret)
		return ret;

	if (!(bmsr & BMSR_LSTATUS)) {
		*link_up = false;
		*speed = EMAC_LINK_SPEED_UNKNOWN;
		return 0;
	}
	*link_up = true;
	ret = emac_phy_read(adpt, phy->addr, MII_PSSR, &pssr);
	if (ret)
		return ret;

	if (!(pssr & PSSR_SPD_DPLX_RESOLVED)) {
		netdev_err(adpt->netdev, "error: speed duplex resolved\n");
		return -EINVAL;
	}

	switch (pssr & PSSR_SPEED) {
	case PSSR_1000MBS:
		if (pssr & PSSR_DPLX)
			*speed = EMAC_LINK_SPEED_1GB_FULL;
		else
			netdev_err(adpt->netdev,
				   "error: 1000M half duplex is invalid");
		break;
	case PSSR_100MBS:
		if (pssr & PSSR_DPLX)
			*speed = EMAC_LINK_SPEED_100_FULL;
		else
			*speed = EMAC_LINK_SPEED_100_HALF;
		break;
	case PSSR_10MBS:
		if (pssr & PSSR_DPLX)
			*speed = EMAC_LINK_SPEED_10_FULL;
		else
			*speed = EMAC_LINK_SPEED_10_HALF;
		break;
	default:
		*speed = EMAC_LINK_SPEED_UNKNOWN;
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* Read speed off the LPA (Link Partner Ability) register */
void emac_phy_link_speed_get(struct emac_adapter *adpt, u32 *speed)
{
	struct emac_phy *phy = &adpt->phy;
	int ret;
	u16 lpa, stat1000;
	bool link;

	if (!phy->external) {
		emac_sgmii_no_ephy_link_check(adpt, speed, &link);
		return;
	}

	ret = emac_phy_read(adpt, phy->addr, MII_LPA, &lpa);
	ret |= emac_phy_read(adpt, phy->addr, MII_STAT1000, &stat1000);
	if (ret)
		return;

	*speed = EMAC_LINK_SPEED_10_HALF;
	if (lpa & LPA_10FULL)
		*speed = EMAC_LINK_SPEED_10_FULL;
	else if (lpa & LPA_10HALF)
		*speed = EMAC_LINK_SPEED_10_HALF;
	else if (lpa & LPA_100FULL)
		*speed = EMAC_LINK_SPEED_100_FULL;
	else if (lpa & LPA_100HALF)
		*speed = EMAC_LINK_SPEED_100_HALF;
	else if (stat1000 & LPA_1000FULL)
		*speed = EMAC_LINK_SPEED_1GB_FULL;
}

/* Read phy configuration and initialize it */
int emac_phy_config(struct platform_device *pdev, struct emac_adapter *adpt)
{
	struct emac_phy *phy = &adpt->phy;
	struct device_node *dt = pdev->dev.of_node;
	int ret;

	phy->external = !of_property_read_bool(dt, "qcom,no-external-phy");

	/* get phy address on MDIO bus */
	if (phy->external) {
		ret = of_property_read_u32(dt, "phy-addr", &phy->addr);
		if (ret)
			return ret;
	} else {
		phy->uses_gpios = false;
	}

	ret = emac_sgmii_config(pdev, adpt);
	if (ret)
		return ret;

	mutex_init(&phy->lock);

	phy->autoneg = true;
	phy->autoneg_advertised = EMAC_LINK_SPEED_DEFAULT;

	return emac_sgmii_init(adpt);
}

int emac_phy_up(struct emac_adapter *adpt)
{
	return emac_sgmii_up(adpt);
}

void emac_phy_down(struct emac_adapter *adpt)
{
	emac_sgmii_down(adpt);
}

void emac_phy_reset(struct emac_adapter *adpt)
{
	emac_sgmii_reset(adpt);
}

void emac_phy_periodic_check(struct emac_adapter *adpt)
{
	emac_sgmii_periodic_check(adpt);
}
