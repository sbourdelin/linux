/* Xilinx GMII2RGMII Converter driver
 *
 * Copyright (C) 2016 Xilinx, Inc.
 *
 * Author: Kedareswara rao Appana <appanad@xilinx.com>
 *
 * Description:
 * This driver is developed for Xilinx GMII2RGMII Converter
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/mii.h>
#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/of_mdio.h>
#include <linux/xilinx_gmii2rgmii.h>

static void xgmii2rgmii_fix_mac_speed(void *priv, unsigned int speed)
{
	struct gmii2rgmii *xphy = (struct xphy *)priv;
	struct phy_device *gmii2rgmii_phydev = xphy->gmii2rgmii_phy_dev;
	u16 gmii2rgmii_reg = 0;

	switch (speed) {
	case 1000:
		gmii2rgmii_reg |= XILINX_GMII2RGMII_SPEED1000;
		break;
	case 100:
		gmii2rgmii_reg |= XILINX_GMII2RGMII_SPEED100;
		break;
	default:
		return;
	}

	xphy->mdio_write(xphy->mii_bus, gmii2rgmii_phydev->mdio.addr,
			 XILINX_GMII2RGMII_REG_NUM,
			 gmii2rgmii_reg);
}

int gmii2rgmii_phyprobe(struct gmii2rgmii *xphy)
{
	struct device_node *phy_node;
	struct phy_device *phydev;
	struct device_node *np = (struct device_node *)xphy->platform_data;

	phy_node = of_parse_phandle(np, "gmii2rgmii-phy-handle", 0);
	if (phy_node) {
		phydev = of_phy_attach(xphy->dev, phy_node, 0, 0);
		if (!phydev) {
			netdev_err(xphy->dev,
				   "%s: no gmii to rgmii converter found\n",
				   xphy->dev->name);
			return -1;
		}
		xphy->gmii2rgmii_phy_dev = phydev;
	}
	xphy->fix_mac_speed = xgmii2rgmii_fix_mac_speed;

	return 0;
}
EXPORT_SYMBOL(gmii2rgmii_phyprobe);

MODULE_DESCRIPTION("Xilinx GMII2RGMII converter driver");
MODULE_LICENSE("GPL");
