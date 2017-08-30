/**
 * drivers/net/ethernet/socionext/netsec/netsec_gmac_access.c
 *
 *  Copyright (C) 2011-2014 Fujitsu Semiconductor Limited.
 *  Copyright (C) 2014 Linaro Ltd  Andy Green <andy.green@linaro.org>
 *  All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 */
#include "netsec.h"

#define TIMEOUT_SPINS_MAC 1000
#define TIMEOUT_SECONDARY_MS_MAC 100

static u32 netsec_clk_type(u32 freq)
{
	if (freq < 35 * NETSEC_CLK_MHZ)
		return NETSEC_GMAC_GAR_REG_CR_25_35_MHZ;
	if (freq < 60 * NETSEC_CLK_MHZ)
		return NETSEC_GMAC_GAR_REG_CR_35_60_MHZ;
	if (freq < 100 * NETSEC_CLK_MHZ)
		return NETSEC_GMAC_GAR_REG_CR_60_100_MHZ;
	if (freq < 150 * NETSEC_CLK_MHZ)
		return NETSEC_GMAC_GAR_REG_CR_100_150_MHZ;
	if (freq < 250 * NETSEC_CLK_MHZ)
		return NETSEC_GMAC_GAR_REG_CR_150_250_MHZ;

	return NETSEC_GMAC_GAR_REG_CR_250_300_MHZ;
}

static int netsec_wait_while_busy(struct netsec_priv *priv, u32 addr, u32 mask)
{
	u32 timeout = TIMEOUT_SPINS_MAC;

	while (--timeout && netsec_readl(priv, addr) & mask)
		;
	if (timeout)
		return 0;

	timeout = TIMEOUT_SECONDARY_MS_MAC;
	while (--timeout && netsec_readl(priv, addr) & mask)
		usleep_range(1000, 2000);

	if (timeout)
		return 0;

	netdev_WARN(priv->ndev, "%s: timeout\n", __func__);

	return -ETIMEDOUT;
}

static int netsec_mac_write(struct netsec_priv *priv, u32 addr, u32 value)
{
	netsec_writel(priv, MAC_REG_DATA, value);
	netsec_writel(priv, MAC_REG_CMD, addr | NETSEC_GMAC_CMD_ST_WRITE);
	return netsec_wait_while_busy(priv,
				      MAC_REG_CMD, NETSEC_GMAC_CMD_ST_BUSY);
}

static int netsec_mac_read(struct netsec_priv *priv, u32 addr, u32 *read)
{
	int ret;

	netsec_writel(priv, MAC_REG_CMD, addr | NETSEC_GMAC_CMD_ST_READ);
	ret = netsec_wait_while_busy(priv,
				     MAC_REG_CMD, NETSEC_GMAC_CMD_ST_BUSY);
	if (ret)
		return ret;

	*read = netsec_readl(priv, MAC_REG_DATA);

	return 0;
}

static int netsec_mac_wait_while_busy(struct netsec_priv *priv,
				      u32 addr, u32 mask)
{
	u32 timeout = TIMEOUT_SPINS_MAC;
	int ret, data;

	do {
		ret = netsec_mac_read(priv, addr, &data);
		if (ret)
			break;
	} while (--timeout && (data & mask));

	if (timeout)
		return 0;

	timeout = TIMEOUT_SECONDARY_MS_MAC;
	do {
		usleep_range(1000, 2000);

		ret = netsec_mac_read(priv, addr, &data);
		if (ret)
			break;
	} while (--timeout && (data & mask));

	if (timeout && !ret)
		return 0;

	netdev_WARN(priv->ndev, "%s: timeout\n", __func__);

	return -ETIMEDOUT;
}

static int netsec_mac_update_to_phy_state(struct netsec_priv *priv)
{
	struct phy_device *phydev = priv->ndev->phydev;
	u32 value = 0;

	value = phydev->duplex ? NETSEC_GMAC_MCR_REG_FULL_DUPLEX_COMMON :
				       NETSEC_GMAC_MCR_REG_HALF_DUPLEX_COMMON;

	if (phydev->speed != SPEED_1000)
		value |= NETSEC_MCR_PS;

	if ((priv->phy_interface != PHY_INTERFACE_MODE_GMII) &&
	    (phydev->speed == SPEED_100))
		value |= NETSEC_GMAC_MCR_REG_FES;

	value |= NETSEC_GMAC_MCR_REG_CST | NETSEC_GMAC_MCR_REG_JE;

	if (priv->phy_interface == PHY_INTERFACE_MODE_RGMII)
		value |= NETSEC_GMAC_MCR_REG_IBN;

	if (netsec_mac_write(priv, GMAC_REG_MCR, value))
		return -ETIMEDOUT;

	priv->actual_link_speed = phydev->speed;
	priv->actual_duplex = phydev->duplex;
	netif_info(priv, drv, priv->ndev, "%s: %uMbps, duplex:%d\n",
		   __func__, phydev->speed, phydev->duplex);

	return 0;
}

/* NB netsec_start_gmac() only called from adjust_link */

int netsec_start_gmac(struct netsec_priv *priv)
{
	struct phy_device *phydev = priv->ndev->phydev;
	u32 value = 0;
	int ret;

	if (priv->desc_ring[NETSEC_RING_TX].running &&
	    priv->desc_ring[NETSEC_RING_RX].running)
		return 0;

	if (!priv->desc_ring[NETSEC_RING_RX].running &&
	    !priv->desc_ring[NETSEC_RING_TX].running) {
		if (phydev->speed != SPEED_1000)
			value = (NETSEC_GMAC_MCR_REG_CST |
				 NETSEC_GMAC_MCR_REG_HALF_DUPLEX_COMMON);

		if (netsec_mac_write(priv, GMAC_REG_MCR, value))
			return -ETIMEDOUT;
		if (netsec_mac_write(priv, GMAC_REG_BMR,
				     NETSEC_GMAC_BMR_REG_RESET))
			return -ETIMEDOUT;

		/* Wait soft reset */
		usleep_range(1000, 5000);

		ret = netsec_mac_read(priv, GMAC_REG_BMR, &value);
		if (ret)
			return ret;
		if (value & NETSEC_GMAC_BMR_REG_SWR)
			return -EAGAIN;

		netsec_writel(priv, MAC_REG_DESC_SOFT_RST, 1);
		if (netsec_wait_while_busy(priv, MAC_REG_DESC_SOFT_RST, 1))
			return -ETIMEDOUT;

		netsec_writel(priv, MAC_REG_DESC_INIT, 1);
		if (netsec_wait_while_busy(priv, MAC_REG_DESC_INIT, 1))
			return -ETIMEDOUT;

		if (netsec_mac_write(priv, GMAC_REG_BMR,
				     NETSEC_GMAC_BMR_REG_COMMON))
			return -ETIMEDOUT;
		if (netsec_mac_write(priv, GMAC_REG_RDLAR, priv->rdlar_pa))
			return -ETIMEDOUT;
		if (netsec_mac_write(priv, GMAC_REG_TDLAR, priv->tdlar_pa))
			return -ETIMEDOUT;
		if (netsec_mac_write(priv, GMAC_REG_MFFR, 0x80000001))
			return -ETIMEDOUT;

		ret = netsec_mac_update_to_phy_state(priv);
		if (ret)
			return ret;

		if (priv->mac_mode.flow_ctrl_enable_flag) {
			netsec_writel(priv, MAC_REG_FLOW_TH,
				      (priv->mac_mode.flow_stop_th << 16) |
				      priv->mac_mode.flow_start_th);
			if (netsec_mac_write(priv, GMAC_REG_FCR,
					     (priv->mac_mode.pause_time << 16) |
					     NETSEC_FCR_RFE | NETSEC_FCR_TFE))
				return -ETIMEDOUT;
		}
	}

	ret = netsec_mac_read(priv, GMAC_REG_OMR, &value);
	if (ret)
		return ret;

	if (!priv->desc_ring[NETSEC_RING_RX].running) {
		value |= NETSEC_GMAC_OMR_REG_SR;
		netsec_start_desc_ring(priv, NETSEC_RING_RX);
	}
	if (!priv->desc_ring[NETSEC_RING_TX].running) {
		value |= NETSEC_GMAC_OMR_REG_ST;
		netsec_start_desc_ring(priv, NETSEC_RING_TX);
	}

	if (netsec_mac_write(priv, GMAC_REG_OMR, value))
		return -ETIMEDOUT;

	netsec_writel(priv, NETSEC_REG_INTEN_SET,
		      NETSEC_IRQ_TX | NETSEC_IRQ_RX);

	return 0;
}

int netsec_stop_gmac(struct netsec_priv *priv)
{
	u32 value;
	int ret;

	ret = netsec_mac_read(priv, GMAC_REG_OMR, &value);
	if (ret)
		return ret;

	if (priv->desc_ring[NETSEC_RING_RX].running) {
		value &= ~NETSEC_GMAC_OMR_REG_SR;
		netsec_stop_desc_ring(priv, NETSEC_RING_RX);
	}
	if (priv->desc_ring[NETSEC_RING_TX].running) {
		value &= ~NETSEC_GMAC_OMR_REG_ST;
		netsec_stop_desc_ring(priv, NETSEC_RING_TX);
	}

	priv->actual_link_speed = 0;
	priv->actual_duplex = false;

	return netsec_mac_write(priv, GMAC_REG_OMR, value);
}

static int netsec_phy_write(struct mii_bus *bus,
			    int phy_addr, int reg, u16 val)
{
	struct netsec_priv *priv = bus->priv;

	if (netsec_mac_write(priv, GMAC_REG_GDR, val))
		return -ETIMEDOUT;
	if (netsec_mac_write(priv, GMAC_REG_GAR,
			     phy_addr << NETSEC_GMAC_GAR_REG_SHIFT_PA |
			     reg << NETSEC_GMAC_GAR_REG_SHIFT_GR |
			     NETSEC_GMAC_GAR_REG_GW | NETSEC_GMAC_GAR_REG_GB) |
			     (netsec_clk_type(priv->freq) <<
			      GMAC_REG_SHIFT_CR_GAR))
		return -ETIMEDOUT;

	return netsec_mac_wait_while_busy(priv, GMAC_REG_GAR,
					  NETSEC_GMAC_GAR_REG_GB);
}

static int netsec_phy_read(struct mii_bus *bus, int phy_addr, int reg_addr)
{
	struct netsec_priv *priv = bus->priv;
	u32 data;
	int ret;

	if (netsec_mac_write(priv, GMAC_REG_GAR, NETSEC_GMAC_GAR_REG_GB |
			     phy_addr << NETSEC_GMAC_GAR_REG_SHIFT_PA |
			     reg_addr << NETSEC_GMAC_GAR_REG_SHIFT_GR |
			     (netsec_clk_type(priv->freq) <<
			      GMAC_REG_SHIFT_CR_GAR)))
		return -ETIMEDOUT;

	ret = netsec_mac_wait_while_busy(priv, GMAC_REG_GAR,
					 NETSEC_GMAC_GAR_REG_GB);
	if (ret)
		return 0;

	ret = netsec_mac_read(priv, GMAC_REG_GDR, &data);
	if (ret)
		return ret;

	return data;
}

int netsec_mii_register(struct netsec_priv *priv)
{
	struct mii_bus *bus = mdiobus_alloc();
	struct resource res;
	int ret;

	if (!bus)
		return -ENOMEM;

	of_address_to_resource(priv->dev->of_node, 0, &res);
	snprintf(bus->id, MII_BUS_ID_SIZE, "%s", priv->dev->of_node->full_name);
	bus->priv = priv;
	bus->name = "SNI NETSEC MDIO";
	bus->read = netsec_phy_read;
	bus->write = netsec_phy_write;
	bus->parent = priv->dev;
	priv->mii_bus = bus;

	ret = of_mdiobus_register(bus, priv->dev->of_node);
	if (ret) {
		mdiobus_free(bus);
		return ret;
	}

	return 0;
}

void netsec_mii_unregister(struct netsec_priv *priv)
{
	mdiobus_unregister(priv->mii_bus);
	mdiobus_free(priv->mii_bus);
	priv->mii_bus = NULL;
}
