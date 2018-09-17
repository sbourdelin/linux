// SPDX-License-Identifier: GPL-2.0
//
// Copyright (c) 2018 MediaTek Inc.
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mii.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/of_mdio.h>
#include <linux/phy.h>
#include <linux/slab.h>

#include "mtk-gmac.h"

static int gmac_mdio_read(struct mii_bus *bus, int phyaddr, int phyreg)
{
	struct net_device *ndev = bus->priv;
	struct gmac_pdata *pdata = netdev_priv(ndev);
	int data;
	u32 value = 0;
	int limit;

	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_PA_POS,
				  MAC_MDIOAR_PA_LEN, phyaddr);
	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_RDA_POS,
				  MAC_MDIOAR_RDA_LEN, phyreg);
	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_CR_POS,
				  MAC_MDIOAR_CR_LEN, 0);
	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_GOC_POS,
				  MAC_MDIOAR_GOC_LEN, 3);
	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_GB_POS,
				  MAC_MDIOAR_GB_LEN, 1);

	limit = 10;
	while (limit-- &&
	       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MDIOAR),
				 MAC_MDIOAR_GB_POS,
				 MAC_MDIOAR_GB_LEN))
		mdelay(10);

	if (limit < 0)
		return -EBUSY;

	GMAC_IOWRITE(pdata, MAC_MDIOAR, value);

	limit = 10;
	while (limit-- &&
	       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MDIOAR),
				 MAC_MDIOAR_GB_POS,
				 MAC_MDIOAR_GB_LEN))
		mdelay(10);

	if (limit < 0)
		return -EBUSY;

	/* Read the data from the MII data register */
	data = (int)GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MDIODR),
				      MAC_MDIODR_GD_POS,
				      MAC_MDIODR_GD_LEN);

	return data;
}

static int gmac_mdio_write(struct mii_bus *bus,
			   int phyaddr,
			   int phyreg,
			   u16 phydata)
{
	struct net_device *ndev = bus->priv;
	struct gmac_pdata *pdata = netdev_priv(ndev);
	u32 value = 0;
	int limit;

	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_PA_POS,
				  MAC_MDIOAR_PA_LEN, phyaddr);
	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_RDA_POS,
				  MAC_MDIOAR_RDA_LEN, phyreg);
	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_CR_POS,
				  MAC_MDIOAR_CR_LEN, 0);
	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_GOC_POS,
				  MAC_MDIOAR_GOC_LEN, 1);
	value = GMAC_SET_REG_BITS(value, MAC_MDIOAR_GB_POS,
				  MAC_MDIOAR_GB_LEN, 1);

	limit = 10;
	while (limit-- &&
	       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MDIOAR),
				 MAC_MDIOAR_GB_POS,
				 MAC_MDIOAR_GB_LEN))
		mdelay(10);

	if (limit < 0)
		return -EBUSY;

	/* Set the MII address register to write */
	GMAC_IOWRITE(pdata, MAC_MDIODR, phydata);
	GMAC_IOWRITE(pdata, MAC_MDIOAR, value);

	limit = 10;
	while (limit-- &&
	       GMAC_GET_REG_BITS(GMAC_IOREAD(pdata, MAC_MDIOAR),
				 MAC_MDIOAR_GB_POS,
				 MAC_MDIOAR_GB_LEN))
		mdelay(10);

	if (limit < 0)
		return -EBUSY;

	return 0;
}

static int gmac_mdio_reset(struct mii_bus *bus)
{
	struct net_device *ndev = bus->priv;
	struct gmac_pdata *pdata = netdev_priv(ndev);

	gpio_direction_output(pdata->phy_rst, 0);

	msleep(20);

	gpio_direction_output(pdata->phy_rst, 1);

	return 0;
}

static void adjust_link(struct net_device *ndev)
{
	struct gmac_pdata *pdata = netdev_priv(ndev);
	struct gmac_hw_ops *hw_ops = &pdata->hw_ops;
	struct phy_device *phydev = pdata->phydev;

	if (!phydev)
		return;

	if (phydev->link) {
		/* Now we make sure that we can be in full duplex mode.
		 * If not, we operate in half-duplex mode
		 */
		if (phydev->duplex)
			hw_ops->set_full_duplex(pdata);
		else
			hw_ops->set_full_duplex(pdata);

		switch (phydev->speed) {
		case SPEED_1000:
			hw_ops->set_gmii_1000_speed(pdata);
			break;
		case SPEED_100:
			hw_ops->set_gmii_100_speed(pdata);
			break;
		case SPEED_10:
			hw_ops->set_gmii_10_speed(pdata);
			break;
		}
	}
}

static int init_phy(struct net_device *ndev)
{
	struct gmac_pdata *pdata = netdev_priv(ndev);
	struct phy_device *phydev = NULL;
	char phy_id_fmt[MII_BUS_ID_SIZE + 3];
	char bus_id[MII_BUS_ID_SIZE];

	snprintf(bus_id, MII_BUS_ID_SIZE, "mtk_gmac-%x", pdata->bus_id);

	snprintf(phy_id_fmt, MII_BUS_ID_SIZE + 3,
		 PHY_ID_FMT, bus_id,
		 pdata->phyaddr);

	phydev = phy_connect(ndev, phy_id_fmt, &adjust_link,
			     pdata->plat->phy_mode);
	if (IS_ERR(phydev)) {
		dev_err(pdata->dev, "%s: Could not attach to PHY\n", ndev->name);
		return PTR_ERR(phydev);
	}

	if (phydev->phy_id == 0) {
		phy_disconnect(phydev);
		return -ENODEV;
	}

	if (pdata->plat->phy_mode == PHY_INTERFACE_MODE_GMII) {
		phydev->supported = PHY_GBIT_FEATURES;
	} else if ((pdata->plat->phy_mode == PHY_INTERFACE_MODE_MII) ||
		(pdata->plat->phy_mode == PHY_INTERFACE_MODE_RMII)) {
		phydev->supported = PHY_BASIC_FEATURES;
	}

	phydev->advertising = phydev->supported;

	pdata->phydev = phydev;
	phy_start(pdata->phydev);

	return 0;
}

int mdio_register(struct net_device *ndev)
{
	struct gmac_pdata *pdata = netdev_priv(ndev);
	struct mii_bus *new_bus = NULL;
	int phyaddr = 0;
	unsigned short phy_detected = 0;
	int ret = 0;

	new_bus = mdiobus_alloc();
	if (!new_bus)
		return -ENOMEM;

	pdata->bus_id = 0x1;
	new_bus->name = "mtk_gmac";
	new_bus->read = gmac_mdio_read;
	new_bus->write = gmac_mdio_write;
	new_bus->reset = gmac_mdio_reset;
	snprintf(new_bus->id, MII_BUS_ID_SIZE, "%s-%x",
		 new_bus->name, pdata->bus_id);
	new_bus->priv = ndev;
	new_bus->phy_mask = 0;
	new_bus->parent = pdata->dev;

	ret = mdiobus_register(new_bus);
	if (ret != 0) {
		dev_err(pdata->dev, "%s: Cannot register as MDIO bus\n", new_bus->name);
		mdiobus_free(new_bus);
		return ret;
	}
	pdata->mii = new_bus;

	for (phyaddr = 0; phyaddr < PHY_MAX_ADDR; phyaddr++) {
		struct phy_device *phydev = mdiobus_get_phy(new_bus, phyaddr);

		if (!phydev)
			continue;

		pdata->phyaddr = phyaddr;

		phy_attached_info(phydev);
		phy_detected = 1;
	}
	if (!phy_detected) {
		dev_warn(pdata->dev, "No PHY found\n");
		ret = -ENODEV;
		goto err_out_phy_connect;
	}

	ret = init_phy(ndev);
	if (unlikely(ret)) {
		dev_err(pdata->dev, "Cannot attach to PHY (error: %d)\n", ret);
		goto err_out_phy_connect;
	}

	return ret;

 err_out_phy_connect:
	mdiobus_unregister(new_bus);
	mdiobus_free(new_bus);
	return ret;
}

void mdio_unregister(struct net_device *ndev)
{
	struct gmac_pdata *pdata = netdev_priv(ndev);

	if (pdata->phydev) {
		phy_stop(pdata->phydev);
		phy_disconnect(pdata->phydev);
		pdata->phydev = NULL;
	}

	mdiobus_unregister(pdata->mii);
	pdata->mii->priv = NULL;
	mdiobus_free(pdata->mii);
	pdata->mii = NULL;
}
