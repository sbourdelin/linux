/*
 *    Based on code from Cortina Systems, Inc.
 *
 *    Copyright (C) 2011 Cortina Systems, Inc.
 *    Copyright (C) 2017 NXP
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 */
#include <linux/module.h>
#include <linux/phy.h>

#define PHY_ID_CS4340	0x13e51002

#define CORTINA_GPIO_GPIO_INTS				0x16D

static int cortina_read_x(struct phy_device *phydev, int off, u16 regnum)
{
	return mdiobus_read(phydev->mdio.bus, phydev->mdio.addr + off,
			    MII_ADDR_C45 | regnum);
}

static int cortina_read(struct phy_device *phydev, u16 regnum)
{
	return cortina_read_x(phydev, 0, regnum);
}

static int cortina_config_aneg(struct phy_device *phydev)
{
	phydev->supported = SUPPORTED_10000baseT_Full;
	phydev->advertising = SUPPORTED_10000baseT_Full;

	return 0;
}

static int cortina_read_status(struct phy_device *phydev)
{
	int gpio_int_status;
	int ret = 0;

	gpio_int_status = cortina_read(phydev, CORTINA_GPIO_GPIO_INTS);
	if (gpio_int_status < 0) {
		ret = gpio_int_status;
		goto err;
	}

	if (gpio_int_status & 0x8) {
		phydev->speed = SPEED_10000;
		phydev->duplex = DUPLEX_FULL;
		phydev->link = 1;
	} else {
		phydev->link = 0;
	}

err:
	return ret;
}

static int cortina_soft_reset(struct phy_device *phydev)
{
	return 0;
}

static struct phy_driver cortina_driver[] = {
{
	.phy_id         = PHY_ID_CS4340,
	.phy_id_mask    = 0xffffffff,
	.name           = "Cortina CS4340",
	.config_aneg    = cortina_config_aneg,
	.read_status    = cortina_read_status,
	.soft_reset     = cortina_soft_reset,
},
};

module_phy_driver(cortina_driver);

static struct mdio_device_id __maybe_unused cortina_tbl[] = {
	{ PHY_ID_CS4340, 0xffffffff},
	{},
};

MODULE_DEVICE_TABLE(mdio, cortina_tbl);
