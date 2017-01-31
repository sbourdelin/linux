/*
 * occ_scom_i2c.c - hwmon OCC driver
 *
 * This file contains the functions for performing SCOM operations over I2C bus
 * to access the OCC.
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

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include "occ_scom_i2c.h"
#include "scom.h"

int occ_i2c_getscom(void *bus, u32 address, u64 *data)
{
	ssize_t rc;
	u64 buf;
	struct i2c_client *client = bus;
	struct i2c_msg msgs[2];

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags & I2C_M_TEN;
	msgs[0].len = sizeof(u32);
	msgs[0].buf = (char *)&address;

	msgs[1].addr = client->addr;
	msgs[1].flags = client->flags & I2C_M_TEN;
	msgs[1].flags |= I2C_M_RD;
	msgs[1].len = sizeof(u64);
	msgs[1].buf = (char *)&buf;

	rc = i2c_transfer(client->adapter, msgs, 2);
	if (rc < 0)
		return rc;

	*data = be64_to_cpu(buf);

	return 0;
}
EXPORT_SYMBOL(occ_i2c_getscom);

int occ_i2c_putscom(void *bus, u32 address, u32 data0, u32 data1)
{
	u32 buf[3];
	ssize_t rc;
	struct i2c_client *client = bus;

	/* send raw data, user can handle endian */
	buf[0] = address;
	buf[1] = data1;
	buf[2] = data0;

	rc = i2c_master_send(client, (const char *)buf, sizeof(u32) * 3);
	if (rc < 0)
		return rc;
	else if (rc != sizeof(u32) * 3)
		return -EIO;

	return 0;
}
EXPORT_SYMBOL(occ_i2c_putscom);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("I2C OCC SCOM transport");
MODULE_LICENSE("GPL");
