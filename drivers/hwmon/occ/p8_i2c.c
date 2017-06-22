/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/unaligned.h>
#include "common.h"
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>

struct p8_i2c_occ {
	struct occ occ;
	struct i2c_client *client;
};

#define to_p8_i2c_occ(x)	container_of((x), struct p8_i2c_occ, occ)

static int p8_i2c_occ_getscom(struct i2c_client *client, u32 address, u8 *data)
{
	ssize_t rc;
	__be64 buf_be;
	u64 buf;
	struct i2c_msg msgs[2];

	/* p8 i2c slave requires shift */
	address <<= 1;

	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags & I2C_M_TEN;
	msgs[0].len = sizeof(u32);
	msgs[0].buf = (char *)&address;


	msgs[1].addr = client->addr;
	msgs[1].flags = (client->flags & I2C_M_TEN) | I2C_M_RD;
	msgs[1].len = sizeof(u64);
	msgs[1].buf = (char *)&buf_be;

	rc = i2c_transfer(client->adapter, msgs, 2);
	if (rc < 0)
		return rc;

	buf = be64_to_cpu(buf_be);
	memcpy(data, &buf, sizeof(u64));

	return 0;
}

static int p8_i2c_occ_putscom(struct i2c_client *client, u32 address, u8 *data)
{
	u32 buf[3];
	ssize_t rc;

	/* p8 i2c slave requires shift */
	address <<= 1;

	buf[0] = address;
	memcpy(&buf[1], &data[4], sizeof(u32));
	memcpy(&buf[2], data, sizeof(u32));

	rc = i2c_master_send(client, (const char *)buf, sizeof(buf));
	if (rc < 0)
		return rc;
	else if (rc != sizeof(buf))
		return -EIO;

	return 0;
}

static int p8_i2c_occ_putscom_u32(struct i2c_client *client, u32 address,
				  u32 data0, u32 data1)
{
	u8 buf[8];

	memcpy(buf, &data0, 4);
	memcpy(buf + 4, &data1, 4);

	return p8_i2c_occ_putscom(client, address, buf);
}

static int p8_i2c_occ_putscom_be(struct i2c_client *client, u32 address,
				 u8 *data)
{
	__be32 data0, data1;

	memcpy(&data0, data, 4);
	memcpy(&data1, data + 4, 4);

	return p8_i2c_occ_putscom_u32(client, address, be32_to_cpu(data0),
				      be32_to_cpu(data1));
}

static int p8_i2c_occ_send_cmd(struct occ *occ, u8 *cmd)
{
	int i, rc;
	unsigned long start;
	u16 data_length;
	struct p8_i2c_occ *p8_i2c_occ = to_p8_i2c_occ(occ);
	struct i2c_client *client = p8_i2c_occ->client;
	struct occ_response *resp = &occ->resp;

	start = jiffies;

	/* set sram address for command */
	rc = p8_i2c_occ_putscom_u32(client, 0x6B070, 0xFFFF6000, 0);
	if (rc)
		goto err;

	/* write command (must already be BE), i2c expects cpu-endian */
	rc = p8_i2c_occ_putscom_be(client, 0x6B075, cmd);
	if (rc)
		goto err;

	/* trigger OCC attention */
	rc = p8_i2c_occ_putscom_u32(client, 0x6B035, 0x20010000, 0);
	if (rc)
		goto err;

retry:
	/* set sram address for response */
	rc = p8_i2c_occ_putscom_u32(client, 0x6B070, 0xFFFF7000, 0);
	if (rc)
		goto err;

	/* read response */
	rc = p8_i2c_occ_getscom(client, 0x6B075, (u8 *)resp);
	if (rc)
		goto err;

	/* check the OCC response */
	switch (resp->return_status) {
	case RESP_RETURN_CMD_IN_PRG:
		if (time_after(jiffies,
			       start + msecs_to_jiffies(OCC_TIMEOUT_MS)))
			rc = -EALREADY;
		else {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(msecs_to_jiffies(OCC_CMD_IN_PRG_MS));

			goto retry;
		}
		break;
	case RESP_RETURN_SUCCESS:
		rc = 0;
		break;
	case RESP_RETURN_CMD_INVAL:
	case RESP_RETURN_CMD_LEN:
	case RESP_RETURN_DATA_INVAL:
	case RESP_RETURN_CHKSUM:
		rc = -EINVAL;
		break;
	case RESP_RETURN_OCC_ERR:
		rc = -EREMOTE;
		break;
	default:
		rc = -EFAULT;
	}

	if (rc < 0) {
		dev_warn(&client->dev, "occ bad response: %d\n",
			 resp->return_status);
		return rc;
	}

	data_length = get_unaligned_be16(&resp->data_length_be);
	if (data_length > OCC_RESP_DATA_BYTES) {
		dev_warn(&client->dev, "occ bad data length: %d\n",
			 data_length);
		return -EDOM;
	}

	/* read remaining response */
	for (i = 8; i < data_length + 7; i += 8) {
		rc = p8_i2c_occ_getscom(client, 0x6B075, ((u8 *)resp) + i);
		if (rc)
			goto err;
	}

	return data_length + 7;

err:
	dev_err(&client->dev, "i2c scom op failed rc: %d\n", rc);
	return rc;
}

static int p8_i2c_occ_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	struct occ *occ;
	struct p8_i2c_occ *p8_i2c_occ = devm_kzalloc(&client->dev,
						     sizeof(*p8_i2c_occ),
						     GFP_KERNEL);
	if (!p8_i2c_occ)
		return -ENOMEM;

	p8_i2c_occ->client = client;
	occ = &p8_i2c_occ->occ;
	occ->bus_dev = &client->dev;
	dev_set_drvdata(&client->dev, occ);

	occ->poll_cmd_data = 0x10;		/* P8 OCC poll data */
	occ->send_cmd = p8_i2c_occ_send_cmd;

	return occ_setup(occ, "p8_occ");
}

static int p8_i2c_occ_remove(struct i2c_client *client)
{
	struct occ *occ = dev_get_drvdata(&client->dev);

	return occ_shutdown(occ);
}

static const struct of_device_id p8_i2c_occ_of_match[] = {
	{ .compatible = "ibm,p8-occ-hwmon" },
	{}
};
MODULE_DEVICE_TABLE(of, p8_i2c_occ_of_match);

static const unsigned short p8_i2c_occ_addr[] = { 0x50, 0x51, I2C_CLIENT_END };

static struct i2c_driver p8_i2c_occ_driver = {
	.class = I2C_CLASS_HWMON,
	.driver = {
		.name = "occ-hwmon",
		.of_match_table = p8_i2c_occ_of_match,
	},
	.probe = p8_i2c_occ_probe,
	.remove = p8_i2c_occ_remove,
	.address_list = p8_i2c_occ_addr,
};

module_i2c_driver(p8_i2c_occ_driver);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("BMC P8 OCC hwmon driver");
MODULE_LICENSE("GPL");
