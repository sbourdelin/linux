/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Serial Camera Control Bus (SCCB) helper functions
 */

#ifndef __SCCB_H__
#define __SCCB_H__

#include <linux/i2c.h>

/**
 * sccb_read_byte - Read data from SCCB slave device
 * @client: Handle to slave device
 * @addr: Register to be read from
 *
 * This executes the 2-phase write transmission cycle that is followed by a
 * 2-phase read transmission cycle, returning negative errno else a data byte
 * received from the device.
 */
static inline int sccb_read_byte(struct i2c_client *client, u8 addr)
{
	u8 val;
	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.len = 1,
			.buf = &addr,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = 1,
			.buf = &val,
		},
	};
	int ret;
	int i;

	i2c_lock_adapter(client->adapter);

	/* Issue two separated requests in order to avoid repeated start */
	for (i = 0; i < 2; i++) {
		ret = __i2c_transfer(client->adapter, &msg[i], 1);
		if (ret != 1)
			break;
	}

	i2c_unlock_adapter(client->adapter);

	return i == 2 ? val : ret;
}

/**
 * sccb_write_byte - Write data to SCCB slave device
 * @client: Handle to slave device
 * @addr: Register to write to
 * @data: Value to be written
 *
 * This executes the SCCB 3-phase write transmission cycle, returning negative
 * errno else zero on success.
 */
static inline int sccb_write_byte(struct i2c_client *client, u8 addr, u8 data)
{
	int ret;
	unsigned char msgbuf[] = { addr, data };

	ret = i2c_master_send(client, msgbuf, 2);
	if (ret < 0)
		return ret;

	return 0;
}

#endif /* __SCCB_H__ */
