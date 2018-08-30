// SPDX-License-Identifier: GPL-2.0
/*
 * UCSI driver for Cypress CCGx Type-C controller
 *
 * Copyright (C) 2017-2018 NVIDIA Corporation. All rights reserved.
 * Author: Ajay Gupta <ajayg@nvidia.com>
 *
 * Some code borrowed from drivers/usb/typec/ucsi/ucsi_acpi.c
 */
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/acpi.h>
#include <linux/i2c.h>
#include <linux/pci.h>
#include "ucsi.h"

struct ucsi_ccg {
	struct device *dev;
	struct ucsi *ucsi;
	struct ucsi_ppm ppm;
	struct i2c_client *client;
	int irq;
};

#define CCGX_I2C_RAB_DEVICE_MODE			0x0
#define CCGX_I2C_RAB_BOOT_MODE_REASON			0x1
#define CCGX_I2C_RAB_READ_SILICON_ID			0x2
#define CCGX_I2C_RAB_INTR_REG				0x6
#define CCGX_I2C_RAB_FW1_VERSION			0x28
#define CCGX_I2C_RAB_FW2_VERSION			0x20
#define CCGX_I2C_RAB_UCSI_CONTROL			0x39
#define CCGX_I2C_RAB_UCSI_CONTROL_STOP			0x2
#define CCGX_I2C_RAB_UCSI_CONTROL_START			0x1
#define CCGX_I2C_RAB_RESPONSE_REG			0x7E
#define CCGX_I2C_RAB_UCSI_DATA_BLOCK			0xf000
#define CCGX_I2C_RAB_RESPONSE_REG_RESET_COMPLETE	0x80

static int ccg_read(struct ucsi_ccg *uc, u16 rab, u8 *data, u32 len)
{
	struct device *dev = uc->dev;
	struct i2c_client *client = uc->client;
	unsigned char buf[2];
	struct i2c_msg msgs[] = {
		{
			.addr	= client->addr,
			.flags  = 0x0,
			.len	= 0x2,
			.buf	= buf,
		},
		{
			.addr	= client->addr,
			.flags  = I2C_M_RD,
			.buf	= data,
		},
	};
	u32 rlen, rem_len = len;
	int err = -EIO;

	while (rem_len > 0) {
		msgs[1].buf = &data[len - rem_len];
		rlen = min_t(u16, rem_len, 4);
		msgs[1].len = rlen;
		buf[0] = (rab >> 0) & 0xff;
		buf[1] = (rab >> 8) & 0xff;
		err = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
		if (err == ARRAY_SIZE(msgs)) {
			err = 0;
		} else if (err >= 0) {
			dev_err(dev, "i2c_transfer failed, err %d\n", err);
			return -EIO;
		}
		rab += rlen;
		rem_len -= rlen;
	}

	return err;
}

static int ccg_write(struct ucsi_ccg *uc, u16 rab, u8 *data, u32 len)
{
	struct device *dev = uc->dev;
	struct i2c_client *client = uc->client;
	unsigned char buf[2];
	struct i2c_msg msgs[] = {
		{
			.addr	= client->addr,
			.flags  = 0x0,
			.len	= 0x2,
			.buf	= buf,
		},
		{
			.addr	= client->addr,
			.flags  = 0x0,
			.buf	= data,
			.len	= len,
		},
		{
			.addr	= client->addr,
			.flags  = I2C_M_STOP,
		},
	};
	int err;

	buf[0] = (rab >> 0) & 0xff;
	buf[1] = (rab >> 8) & 0xff;
	err = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (err == ARRAY_SIZE(msgs)) {
		err = 0;
	} else if (err >= 0) {
		dev_err(dev, "i2c_transfer failed, err %d\n", err);
		return -EIO;
	}

	return err;
}

static int ucsi_ccg_init(struct ucsi_ccg *uc)
{
	struct device *dev = uc->dev;
	u8 data[64];
	int i, err;

	/* selectively issue device reset
	 * - if RESPONSE register is RESET_COMPLETE, do not issue device reset
	 *   (will cause usb device disconnect / reconnect)
	 * - if RESPONSE register is not RESET_COMPLETE, issue device reset
	 *   (causes PPC to resync device connect state by re-issuing
	 *   set mux command)
	 */
	data[0] = 0x00;
	data[1] = 0x00;

	err = ccg_read(uc, CCGX_I2C_RAB_RESPONSE_REG, data, 0x2);
	if (err < 0) {
		dev_err(dev, "ccg_read failed, err %d\n", err);
		return -EIO;
	}

	memset(data, 0, sizeof(data));
	err = ccg_read(uc, CCGX_I2C_RAB_DEVICE_MODE, data, sizeof(data));
	if (err < 0) {
		dev_err(dev, "ccg_read failed, err %d\n", err);
		return -EIO;
	}

	dev_info(dev, "Silicon id %02x %02x",
		 data[CCGX_I2C_RAB_READ_SILICON_ID + 0],
		 data[CCGX_I2C_RAB_READ_SILICON_ID + 1]);

	dev_info(dev, "FW1 version %02x %02x %02x %02x %02x %02x %02x %02x\n",
		 data[CCGX_I2C_RAB_FW1_VERSION + 0],
		 data[CCGX_I2C_RAB_FW1_VERSION + 1],
		 data[CCGX_I2C_RAB_FW1_VERSION + 2],
		 data[CCGX_I2C_RAB_FW1_VERSION + 3],
		 data[CCGX_I2C_RAB_FW1_VERSION + 4],
		 data[CCGX_I2C_RAB_FW1_VERSION + 5],
		 data[CCGX_I2C_RAB_FW1_VERSION + 6],
		 data[CCGX_I2C_RAB_FW1_VERSION + 7]);

	dev_info(dev, "FW2 version %02x %02x %02x %02x %02x %02x %02x %02x\n",
		 data[CCGX_I2C_RAB_FW2_VERSION + 0],
		 data[CCGX_I2C_RAB_FW2_VERSION + 1],
		 data[CCGX_I2C_RAB_FW2_VERSION + 2],
		 data[CCGX_I2C_RAB_FW2_VERSION + 3],
		 data[CCGX_I2C_RAB_FW2_VERSION + 4],
		 data[CCGX_I2C_RAB_FW2_VERSION + 5],
		 data[CCGX_I2C_RAB_FW2_VERSION + 6],
		 data[CCGX_I2C_RAB_FW2_VERSION + 7]);

	data[0] = 0x0;
	data[1] = 0x0;
	err = ccg_read(uc, CCGX_I2C_RAB_RESPONSE_REG, data, 0x2);
	if (err < 0) {
		dev_err(dev, "ccg_read failed, err %d\n", err);
		return -EIO;
	}

	data[0] = CCGX_I2C_RAB_UCSI_CONTROL_STOP;
	err = ccg_write(uc, CCGX_I2C_RAB_UCSI_CONTROL, data, 0x1);
	if (err < 0) {
		dev_err(dev, "ccg_write failed, err %d\n", err);
		return -EIO;
	}

	data[0] = CCGX_I2C_RAB_UCSI_CONTROL_START;
	err = ccg_write(uc, CCGX_I2C_RAB_UCSI_CONTROL, data, 0x1);
	if (err < 0) {
		dev_err(dev, "ccg_write failed, err %d\n", err);
		return -EIO;
	}

	/* flush CCGx RESPONSE queue by acking interrupts
	 * - above ucsi control register write will push response
	 * which must be flushed
	 * - affects f/w update which reads response register
	 */
	data[0] = 0xff;
	for (i = 0; (i < 10) && (data[0] != 0x00); i++) {
		err = ccg_write(uc, CCGX_I2C_RAB_INTR_REG, data, 0x1);
		if (err < 0) {
			dev_err(dev, "ccg_write failed, err %d\n", err);
			return -EIO;
		}

		usleep_range(10000, 11000);

		err = ccg_read(uc, CCGX_I2C_RAB_INTR_REG, data, 0x1);
		if (err < 0) {
			dev_err(dev, "ccg_read failed, err %d\n", err);
			return -EIO;
		}
	}
	return 0;
}

static int ucsi_ccg_send_data(struct ucsi_ccg *uc)
{
	int err;
	unsigned char buf[4] = {
		0x20, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
		0x8, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
	};
	unsigned char buf1[16];
	unsigned char buf2[8];

	memcpy(&buf1[0], ((const void *)uc->ppm.data) + 0x20, sizeof(buf1));
	memcpy(&buf2[0], ((const void *)uc->ppm.data) + 0x8, sizeof(buf2));

	err = ccg_write(uc, *(u16 *)buf, buf1, sizeof(buf1));
	if (err < 0) {
		dev_err(uc->dev, "ccg_write failed, err %d\n", err);
		return -EIO;
	}

	err = ccg_write(uc, *(u16 *)(buf + 2), buf2, sizeof(buf2));
	if (err < 0) {
		dev_err(uc->dev, "ccg_write failed, err %d\n", err);
		return -EIO;
	}

	return err;
}

static int ucsi_ccg_recv_data(struct ucsi_ccg *uc)
{
	static int first = 1;
	int err;
	unsigned char buf[6] = {
		0x0, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
		0x4, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
		0x10, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
	};
	u8 *ppm = (u8 *)uc->ppm.data;

	if (first) {
		err = ccg_read(uc, *(u16 *)buf, ppm, 0x2);
		if (err < 0) {
			dev_err(uc->dev, "ccg_read failed, err %d\n", err);
			return -EIO;
		}
		first = 1;
	}

	err = ccg_read(uc, *(u16 *)(buf + 2), ppm + 0x4, 0x4);
	if (err < 0) {
		dev_err(uc->dev, "ccg_read failed, err %d\n", err);
		return -EIO;
	}

	err = ccg_read(uc, *(u16 *)(buf + 4), ppm + 0x10, 0x10);
	if (err < 0) {
		dev_err(uc->dev, "ccg_read failed, err %d\n", err);
		return -EIO;
	}

	return err;
}

static int ucsi_ccg_ack_interrupt(struct ucsi_ccg *uc)
{
	int err;
	unsigned char buf[3] = {(CCGX_I2C_RAB_INTR_REG & 0xff),
		(CCGX_I2C_RAB_INTR_REG >> 8), 0x0};

	err = ccg_read(uc, *(u16 *)buf, &buf[2], 0x1);
	if (err < 0) {
		dev_err(uc->dev, "ccg_read failed, err %d\n", err);
		return -EIO;
	}

	err = ccg_write(uc, *(u16 *)buf, &buf[2], 0x1);
	if (err < 0) {
		dev_err(uc->dev, "ccg_read failed, err %d\n", err);
		return -EIO;
	}

	return err;
}

static int ucsi_ccg_sync(struct ucsi_ppm *ppm)
{
	struct ucsi_ccg *uc = container_of(ppm,
		struct ucsi_ccg, ppm);
	int err;

	err = ucsi_ccg_recv_data(uc);
	if (err < 0) {
		dev_err(uc->dev, "ucsi_ccg_recv_data() err %d\n", err);
		return 0;
	}

	/* ack interrupt to allow next command to run */
	err = ucsi_ccg_ack_interrupt(uc);
	if (err < 0)
		dev_err(uc->dev, "ucsi_ccg_ack_interrupt() err %d\n", err);

	return 0;
}

static int ucsi_ccg_cmd(struct ucsi_ppm *ppm, struct ucsi_control *ctrl)
{
	struct ucsi_ccg *uc = container_of(ppm,
		struct ucsi_ccg, ppm);
	int err = 0;

	ppm->data->ctrl.raw_cmd = ctrl->raw_cmd;
	err = ucsi_ccg_send_data(uc);

	return err;
}

static irqreturn_t ccg_irq_handler(int irq, void *data)
{
	struct ucsi_ccg *uc = data;

	ucsi_notify(uc->ucsi);

	return IRQ_HANDLED;
}

static int ucsi_ccg_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ucsi_ccg *uc;
	int status;

	uc = devm_kzalloc(dev, sizeof(*uc), GFP_KERNEL);
	if (!uc)
		return -ENOMEM;

	uc->ppm.data = devm_kzalloc(dev, sizeof(struct ucsi_data), GFP_KERNEL);
	if (!uc->ppm.data)
		return -ENOMEM;

	uc->ppm.cmd = ucsi_ccg_cmd;
	uc->ppm.sync = ucsi_ccg_sync;
	uc->dev = dev;
	uc->client = client;

	/* reset ccg device and initialize ucsi */
	status = ucsi_ccg_init(uc);
	if (status < 0) {
		dev_err(uc->dev, "ucsi_ccg_init failed - %d\n", status);
		return status;
	}

	uc->irq = client->irq;

	status = devm_request_threaded_irq(dev, uc->irq, NULL, ccg_irq_handler,
					   IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
					   dev_name(dev), uc);
	if (status < 0) {
		dev_err(uc->dev, "request_threaded_irq failed - %d\n", status);
		return status;
	}

	uc->ucsi = ucsi_register_ppm(dev, &uc->ppm);
	if (IS_ERR(uc->ucsi)) {
		dev_err(uc->dev, "ucsi_register_ppm failed\n");
		return PTR_ERR(uc->ucsi);
	}

	i2c_set_clientdata(client, uc);
	return 0;
}

static int ucsi_ccg_remove(struct i2c_client *client)
{
	struct ucsi_ccg *uc = i2c_get_clientdata(client);

	ucsi_unregister_ppm(uc->ucsi);

	return 0;
}

static const struct i2c_device_id ucsi_ccg_device_id[] = {
	{"ccgx-ucsi", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, ucsi_ccg_device_id);

static struct i2c_driver ucsi_ccg_driver = {
	.driver = {
		.name = "ucsi_ccg",
	},
	.probe = ucsi_ccg_probe,
	.remove = ucsi_ccg_remove,
	.id_table = ucsi_ccg_device_id,
};

module_i2c_driver(ucsi_ccg_driver);

MODULE_AUTHOR("Ajay Gupta <ajayg@nvidia.com>");
MODULE_DESCRIPTION("UCSI driver for Cypress CCGx Type-C controller");
MODULE_LICENSE("GPL v2");
