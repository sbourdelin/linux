// SPDX-License-Identifier: GPL-2.0
/* tja1100.c: TJA1100 BoardR-REACH PHY driver.
 *
 * Copyright (c) 2017 Kirill Kranke <kirill.kranke@gmail.com>
 * Author: Kirill Kranke <kirill.kranke@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/phy.h>

static int tja1100_phy_config_init(struct phy_device *phydev)
{
	phydev->autoneg = AUTONEG_DISABLE;
	phydev->speed = SPEED_100;
	phydev->duplex = DUPLEX_FULL;

	return 0;
}

static int tja1100_phy_config_aneg(struct phy_device *phydev)
{
	if (phydev->autoneg == AUTONEG_ENABLE) {
		phydev_err(phydev, "autonegotiation is not supported\n");
		return -EINVAL;
	}

	if (phydev->speed != SPEED_100 || phydev->duplex != DUPLEX_FULL) {
		phydev_err(phydev, "only 100MBps Full Duplex allowed\n");
		return -EINVAL;
	}

	return 0;
}

static struct phy_driver tja1100_phy_driver[] = {
	{
		.phy_id = 0x0180dc48,
		.phy_id_mask = 0xfffffff0,
		.name = "NXP TJA1100",

		/* TJA1100 has only 100BASE-BroadR-REACH ability specified
		 * at MII_ESTATUS register. Standard modes are not
		 * supported. Therefore BroadR-REACH allow only 100Mbps
		 * full duplex without autoneg.
		 */
		.features = SUPPORTED_100baseT_Full | SUPPORTED_MII,

		.config_aneg = tja1100_phy_config_aneg,
		.config_init = tja1100_phy_config_init,

		.suspend = genphy_suspend,
		.resume = genphy_resume,
	}
};

module_phy_driver(tja1100_phy_driver);

MODULE_DESCRIPTION("NXP TJA1100 driver");
MODULE_AUTHOR("Kirill Kranke <kkranke@topcon.com>");
MODULE_LICENSE("GPL");

static struct mdio_device_id __maybe_unused nxp_tbl[] = {
	{ 0x0180dc48, 0xfffffff0 },
	{}
};

MODULE_DEVICE_TABLE(mdio, nxp_tbl);
