/*
 * p8_occ_i2c.c - hwmon OCC driver
 *
 * This file contains the i2c layer for accessing the P8 OCC over i2c bus.
 *
 * Copyright 2016 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/device.h>

#include "scom.h"
#include "occ_scom_i2c.h"
#include "occ_p8.h"
#include "occ_sysfs.h"

#define P8_OCC_I2C_NAME	"p8-occ-i2c"

static char *caps_sensor_names[] = {
	"curr_powercap",
	"curr_powerreading",
	"norm_powercap",
	"max_powercap",
	"min_powercap",
	"user_powerlimit"
};

int p8_i2c_getscom(void *bus, u32 address, u64 *data)
{
	/* P8 i2c slave requires address to be shifted by 1 */
	address = address << 1;

	return occ_i2c_getscom(bus, address, data);
}

int p8_i2c_putscom(void *bus, u32 address, u32 data0, u32 data1)
{
	/* P8 i2c slave requires address to be shifted by 1 */
	address = address << 1;

	return occ_i2c_putscom(bus, address, data0, data1);
}

static struct occ_bus_ops p8_bus_ops = {
	.getscom = p8_i2c_getscom,
	.putscom = p8_i2c_putscom,
};

static struct occ_sysfs_config p8_sysfs_config = {
	.num_caps_fields = ARRAY_SIZE(caps_sensor_names),
	.caps_names = caps_sensor_names,
};

static int p8_occ_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct occ *occ;
	struct occ_sysfs *hwmon;

	occ = p8_occ_start(&client->dev, client, &p8_bus_ops);
	if (IS_ERR(occ))
		return PTR_ERR(occ);

	hwmon = occ_sysfs_start(&client->dev, occ, &p8_sysfs_config);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);

	i2c_set_clientdata(client, hwmon);

	return 0;
}

static int p8_occ_remove(struct i2c_client *client)
{
	int rc;
	struct occ_sysfs *hwmon = i2c_get_clientdata(client);
	struct occ *occ = hwmon->occ;

	rc = occ_sysfs_stop(&client->dev, hwmon);

	rc = rc || p8_occ_stop(occ);

	return rc;
}

/* used by old-style board info. */
static const struct i2c_device_id occ_ids[] = {
	{ P8_OCC_I2C_NAME, 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, occ_ids);

/* used by device table */
static const struct of_device_id occ_of_match[] = {
	{ .compatible = "ibm,p8-occ-i2c" },
	{}
};
MODULE_DEVICE_TABLE(of, occ_of_match);

/*
 * i2c-core uses i2c-detect() to detect device in below address list.
 * If exists, address will be assigned to client.
 * It is also possible to read address from device table.
 */
static const unsigned short normal_i2c[] = {0x50, 0x51, I2C_CLIENT_END };

static struct i2c_driver p8_occ_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name = P8_OCC_I2C_NAME,
		.pm = NULL,
		.of_match_table = occ_of_match,
	},
	.probe = p8_occ_probe,
	.remove = p8_occ_remove,
	.id_table = occ_ids,
	.address_list = normal_i2c,
};

module_i2c_driver(p8_occ_driver);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("BMC P8 OCC hwmon driver");
MODULE_LICENSE("GPL");
