// SPDX-License-Identifier: GPL-2.0
/*
 * UCSI I2C driver for Cypress CCGx Type-C controller
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

struct ucsi_i2c_ccg {
	struct device *dev;
	struct ucsi *ucsi;
	struct ucsi_ppm ppm;
	struct i2c_client *client;
	int irq;
	bool wake_enabled;
	unsigned char ver;
};

#define CCGX_I2C_RAB_DEVICE_MODE			0x0000U
#define CCGX_I2C_RAB_BOOT_MODE_REASON			0x0001U
#define CCGX_I2C_RAB_READ_SILICON_ID			0x0002U
#define CCGX_I2C_RAB_INTR_REG				0x0006U
#define CCGX_I2C_RAB_RESET				0x0008U
#define CCGX_I2C_RAB_READ_ALL_VERSION			0x0010U
#define CCGX_I2C_RAB_READ_ALL_VERSION_BOOTLOADER \
			(CCGX_I2C_RAB_READ_ALL_VERSION + 0x00)
#define CCGX_I2C_RAB_READ_ALL_VERSION_BOOTLOADER_BASE \
			(CCGX_I2C_RAB_READ_ALL_VERSION_BOOTLOADER + 0)
#define CCGX_I2C_RAB_READ_ALL_VERSION_BOOTLOADER_FW \
			(CCGX_I2C_RAB_READ_ALL_VERSION_BOOTLOADER + 4)
#define CCGX_I2C_RAB_READ_ALL_VERSION_APP \
			(CCGX_I2C_RAB_READ_ALL_VERSION + 0x08)
#define CCGX_I2C_RAB_READ_ALL_VERSION_APP_BASE \
			(CCGX_I2C_RAB_READ_ALL_VERSION_APP + 0)
#define CCGX_I2C_RAB_READ_ALL_VERSION_APP_FW \
			(CCGX_I2C_RAB_READ_ALL_VERSION_APP + 4)
#define CCGX_I2C_RAB_FW2_VERSION			0x0020U
#define CCGX_I2C_RAB_PDPORT_ENABLE			0x002CU
#define CCGX_I2C_RAB_UCSI_STATUS			0x0038U
#define CCGX_I2C_RAB_UCSI_CONTROL			0x0039U
#define CCGX_I2C_RAB_UCSI_CONTROL_STOP			0x2U
#define CCGX_I2C_RAB_UCSI_CONTROL_START			0x1U
#define CCGX_I2C_RAB_HPI_VERSION			0x003CU
#define CCGX_I2C_RAB_RESPONSE_REG			0x007EU
#define CCGX_I2C_RAB_DM_CONTROL_1			0x1000U
#define CCGX_I2C_RAB_WRITE_DATA_MEMORY_1		0x1800U
#define CCGX_I2C_RAB_DM_CONTROL_2			0x2000U
#define CCGX_I2C_RAB_WRITE_DATA_MEMORY_2		0x2800U
#define CCGX_I2C_RAB_UCSI_DATA_BLOCK			0xf000U

#define CCGX_I2C_RAB_RESPONSE_REG_RESET_COMPLETE	0x80

static int ccg_read(struct ucsi_i2c_ccg *ui, u16 rab, u8 *data, u32 len)
{
	struct device *dev = ui->dev;
	struct i2c_client *client = ui->client;
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
	int err;

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
			dev_err(dev, "%s i2c_transfer failed, err %d\n",
				__func__, err);
			return -EIO;
		}
		rab += rlen;
		rem_len -= rlen;
	}

	return err;
}

static int ccg_write(struct ucsi_i2c_ccg *ui, u16 rab, u8 *data, u32 len)
{
	struct device *dev = ui->dev;
	struct i2c_client *client = ui->client;
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
		dev_err(dev, "%s i2c_transfer failed, err %d\n",
			__func__, err);
		return -EIO;
	}

	return err;
}

static int ucsi_i2c_ccg_init(struct ucsi_i2c_ccg *ui)
{
	struct device *dev = ui->dev;
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

	err = ccg_read(ui, CCGX_I2C_RAB_RESPONSE_REG, data, 0x2);
	if (err < 0) {
		dev_err(dev, "%s ccg_read failed, err %d\n", __func__, err);
		return -EIO;
	}

	dev_info(dev, "CCGX_I2C_RAB_RESPONSE_REG %02x", data[0]);

	/* read device mode */
	memset(data, 0, sizeof(data));

	err = ccg_read(ui, CCGX_I2C_RAB_DEVICE_MODE, data, sizeof(data));
	if (err < 0) {
		dev_err(dev, "%s ccg_read failed, err %d\n", __func__, err);
		return -EIO;
	}

	dev_info(dev, "[DEVICE_MODE] %02x (HPIv%c) (Flash row size %d)\n",
		data[CCGX_I2C_RAB_DEVICE_MODE],
		((data[CCGX_I2C_RAB_DEVICE_MODE] >> 7) & 0x01) ? '2' : '1',
		((data[CCGX_I2C_RAB_DEVICE_MODE] >> 4) & 0x03) ? 256 : 128);

	dev_info(dev, "(PD ports %d) (Firmware mode %d)\n",
		((data[CCGX_I2C_RAB_DEVICE_MODE] >> 2) & 0x03) ? 2 : 1,
		((data[CCGX_I2C_RAB_DEVICE_MODE] >> 0) & 0x03));

	dev_info(dev, "[BOOT_MODE_REASON] %02x (Boot mode requested %d)\n",
		data[CCGX_I2C_RAB_BOOT_MODE_REASON],
		((data[CCGX_I2C_RAB_BOOT_MODE_REASON] >> 0) & 0x01) ? 1 : 0);

	dev_info(dev, "(FW1 valid %d) (FW2 valid %d)\n",
		((data[CCGX_I2C_RAB_BOOT_MODE_REASON] >> 2) & 0x01) ? 1 : 0,
		((data[CCGX_I2C_RAB_BOOT_MODE_REASON] >> 3) & 0x01) ? 1 : 0);

	dev_info(dev, "[READ_SILICON_ID] %02x %02x",
		data[CCGX_I2C_RAB_READ_SILICON_ID+0],
		data[CCGX_I2C_RAB_READ_SILICON_ID+1]);

	dev_info(dev, "[READ_ALL_VERSION][BOOTLOADER]\n");
	dev_info(dev, "%02x %02x %02x %02x %02x %02x %02x %02x\n",
		data[CCGX_I2C_RAB_READ_ALL_VERSION+0],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+1],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+2],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+3],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+4],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+5],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+6],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+7]);

	dev_info(dev, "[READ_ALL_VERSION][FW1]\n");
	dev_info(dev, "%02x %02x %02x %02x %02x %02x %02x %02x\n",
		data[CCGX_I2C_RAB_READ_ALL_VERSION+8+0],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+8+1],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+8+2],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+8+3],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+8+4],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+8+5],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+8+6],
		data[CCGX_I2C_RAB_READ_ALL_VERSION+8+7]);

	dev_info(dev, "[FW2_VERSION] %02x %02x %02x %02x %02x %02x %02x %02x\n",
		data[CCGX_I2C_RAB_FW2_VERSION+0],
		data[CCGX_I2C_RAB_FW2_VERSION+1],
		data[CCGX_I2C_RAB_FW2_VERSION+2],
		data[CCGX_I2C_RAB_FW2_VERSION+3],
		data[CCGX_I2C_RAB_FW2_VERSION+4],
		data[CCGX_I2C_RAB_FW2_VERSION+5],
		data[CCGX_I2C_RAB_FW2_VERSION+6],
		data[CCGX_I2C_RAB_FW2_VERSION+7]);

	/* read response register */
	data[0] = 0x0;
	data[1] = 0x0;

	err = ccg_read(ui, CCGX_I2C_RAB_RESPONSE_REG, data, 0x2);
	if (err < 0) {
		dev_err(dev, "%s ccg_read failed, err %d\n", __func__, err);
		return -EIO;
	}

	if (data[0] != CCGX_I2C_RAB_RESPONSE_REG_RESET_COMPLETE) {
		dev_info(dev, "response (%02x %02x) != reset_complete",
			data[0], data[1]);
	}

	/* stop UCSI */
	err = ccg_write(ui, CCGX_I2C_RAB_UCSI_CONTROL, data, 0x1);
	if (err < 0) {
		dev_err(dev, "%s ccg_write failed, err %d\n", __func__, err);
		return -EIO;
	}

	msleep(500);

	/* start UCSI */
	data[0] = CCGX_I2C_RAB_UCSI_CONTROL_START;
	err = ccg_write(ui, CCGX_I2C_RAB_UCSI_CONTROL, data, 0x1);
	if (err < 0) {
		dev_err(dev, "%s ccg_write failed, err %d\n", __func__, err);
		return -EIO;
	}

	msleep(500);

	/* test read-1 */
	err = ccg_read(ui, CCGX_I2C_RAB_UCSI_DATA_BLOCK, data, 0x2);
	if (err < 0) {
		dev_err(dev, "%s ccg_read failed, err %d\n", __func__, err);
		return -EIO;
	}

	/* test read-2 */
	err = ccg_read(ui, 0xf004, data, 0x4);
	if (err < 0) {
		dev_err(dev, "%s ccg_read failed, err %d\n", __func__, err);
		return -EIO;
	}

	/* test read-3 */
	err = ccg_read(ui, 0xf010, data, 0x10);
	if (err < 0) {
		dev_err(dev, "%s ccg_read failed, err %d\n", __func__, err);
		return -EIO;
	}

	/* flush CCGx RESPONSE queue by acking interrupts
	 * - above ucsi control register write will push response
	 * which must be flushed
	 * - affects f/w update which reads response register
	 */
	data[0] = 0xff;
	for (i = 0; (i < 10) && (data[0] != 0x00); i++) {
		dev_dbg(dev, "flushing response %02x\n", data[0]);

		err = ccg_write(ui, CCGX_I2C_RAB_INTR_REG, data, 0x1);
		if (err < 0) {
			dev_err(dev, "%s ccg_write failed, err %d\n",
			__func__, err);
			return -EIO;
		}

		usleep_range(10000, 11000);

		err = ccg_read(ui, CCGX_I2C_RAB_INTR_REG, data, 0x1);
		if (err < 0) {
			dev_err(dev, "%s ccg_read failed, err %d\n",
			__func__, err);
			return -EIO;
		}
	}

	/* get i2c slave firmware version
	 * - [0..1] = Application name (ASCII "nb" for notebook)
	 * - [2] = external circuil specific version
	 * - [3] bit 0...3 = minor version
	 * - [3] bit 4...7 = major version
	 */
	err = ccg_read(ui, 0x0, data, 0x4);
	if (err < 0) {
		dev_err(dev, "%s ccg_read failed, err %d\n", __func__, err);
		return -EIO;
	}

	ui->ver = data[3];

	dev_info(dev, "version %d.%d\n", (ui->ver >> 4) & 0x0f,
		(ui->ver >> 0) & 0x0f);

	return 0;
}

static int ucsi_i2c_ccg_send_data(struct ucsi_i2c_ccg *ui)
{
	int err;
	unsigned char buf[4] = {
		0x20, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
		0x8, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
	};
	unsigned char buf1[16];
	unsigned char buf2[8];

	memcpy(&buf1[0], ((const void *) ui->ppm.data) + 0x20, sizeof(buf1));
	memcpy(&buf2[0], ((const void *) ui->ppm.data) + 0x8, sizeof(buf2));

	err = ccg_write(ui, *(u16 *)buf, buf1, sizeof(buf1));
	if (err < 0) {
		dev_err(ui->dev, "%s ccg_write failed, err %d\n",
			__func__, err);
		return -EIO;
	}

	err = ccg_write(ui, *(u16 *)(buf+2), buf2, sizeof(buf2));
	if (err < 0) {
		dev_err(ui->dev, "%s ccg_write failed, err %d\n",
			__func__, err);
		return -EIO;
	}

	return err;
}

static int ucsi_i2c_ccg_recv_data(struct ucsi_i2c_ccg *ui)
{
	static int first = 1;
	int err;
	unsigned char buf[6] = {
		0x0, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
		0x4, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
		0x10, (CCGX_I2C_RAB_UCSI_DATA_BLOCK >> 8),
	};
	u8 *ppm = (u8 *)ui->ppm.data;

	if (first) {
		err = ccg_read(ui, *(u16 *)buf, ppm, 0x2);
		if (err < 0) {
			dev_err(ui->dev, "%s ccg_read failed, err %d\n",
				__func__, err);
			return -EIO;
		}
		first = 1;
	}

	err = ccg_read(ui, *(u16 *)(buf + 2), ppm + 0x4, 0x4);
	if (err < 0) {
		dev_err(ui->dev, "%s ccg_read failed, err %d\n",
				__func__, err);
		return -EIO;
	}

	err = ccg_read(ui, *(u16 *)(buf + 4), ppm + 0x10, 0x10);
	if (err < 0) {
		dev_err(ui->dev, "%s ccg_read failed, err %d\n",
				__func__, err);
		return -EIO;
	}

	return err;
}

static int ucsi_i2c_ccg_ack_interrupt(struct ucsi_i2c_ccg *ui)
{
	int err;
	unsigned char buf[3] = {(CCGX_I2C_RAB_INTR_REG & 0xff),
		(CCGX_I2C_RAB_INTR_REG >> 8), 0x0};

	err = ccg_read(ui, *(u16 *)buf, &buf[2], 0x1);
	if (err < 0) {
		dev_err(ui->dev, "%s ccg_read failed, err %d\n",
				__func__, err);
		return -EIO;
	}

	err = ccg_write(ui, *(u16 *)buf, &buf[2], 0x1);
	if (err < 0) {
		dev_err(ui->dev, "%s ccg_read failed, err %d\n",
				__func__, err);
		return -EIO;
	}

	return err;
}

static int ucsi_i2c_ccg_sync(struct ucsi_ppm *ppm)
{
	struct ucsi_i2c_ccg *ui = container_of(ppm,
		struct ucsi_i2c_ccg, ppm);
	int err;

	err = ucsi_i2c_ccg_recv_data(ui);
	if (err < 0) {
		dev_err(ui->dev, "%s: ucsi_i2c_ccg_recv_data() err %d\n",
			__func__, err);
		goto exit;
	}

	/* ack interrupt to allow next command to run */
	err = ucsi_i2c_ccg_ack_interrupt(ui);
	if (err < 0) {
		dev_err(ui->dev, "%s: ucsi_i2c_ccg_ack_interrupt() err %d\n",
			__func__, err);
	}
exit:
	return 0;
}

static int ucsi_i2c_ccg_cmd(struct ucsi_ppm *ppm, struct ucsi_control *ctrl)
{
	struct ucsi_i2c_ccg *ui = container_of(ppm,
		struct ucsi_i2c_ccg, ppm);
	int err = 0;

	ppm->data->ctrl.raw_cmd = ctrl->raw_cmd;
	err = ucsi_i2c_ccg_send_data(ui);

	return err;
}

static irqreturn_t i2c_ccg_irq_handler(int irq, void *data)
{
	struct ucsi_i2c_ccg *ui = data;

	dev_dbg(ui->dev, "%s irq %d data %p ui %p\n",
		__func__, irq, data, ui);

	if (!ui) {
		dev_err(ui->dev, "%s: !ui\n", __func__);
		return IRQ_HANDLED;
	}

	ucsi_notify(ui->ucsi);

	return IRQ_HANDLED;
}

static int ucsi_i2c_ccg_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ucsi_i2c_ccg *ui;
	int status;

	ui = devm_kzalloc(dev, sizeof(*ui), GFP_KERNEL);
	if (!ui)
		return -ENOMEM;

	ui->ppm.data = devm_kzalloc(dev, sizeof(struct ucsi_data), GFP_KERNEL);
	if (!ui->ppm.data)
		return -ENOMEM;

	ui->ppm.cmd = ucsi_i2c_ccg_cmd;
	ui->ppm.sync = ucsi_i2c_ccg_sync;
	ui->dev = dev;
	ui->client = client;

	/* reset i2c device and initialize ucsi */
	status = ucsi_i2c_ccg_init(ui);
	if (status < 0) {
		dev_err(ui->dev, "%s: ucsi_i2c_ccg_init failed - %d\n",
			__func__, status);
		return status;
	}

	ui->irq = client->irq;

	status = devm_request_threaded_irq(dev, ui->irq, NULL,
		i2c_ccg_irq_handler, IRQF_ONESHOT | IRQF_TRIGGER_HIGH,
		dev_name(dev), ui);
	if (status < 0) {
		dev_err(ui->dev, "%s: request_irq failed - %d\n",
			__func__, status);
		return status;
	}

	ui->ucsi = ucsi_register_ppm(dev, &ui->ppm);
	if (IS_ERR(ui->ucsi)) {
		dev_err(ui->dev, "ucsi_register_ppm failed\n");
		return PTR_ERR(ui->ucsi);
	}

	i2c_set_clientdata(client, ui);
	return 0;
}

static int ucsi_i2c_ccg_remove(struct i2c_client *client)
{
	struct ucsi_i2c_ccg *ui = i2c_get_clientdata(client);

	ucsi_unregister_ppm(ui->ucsi);

	return 0;
}

static int ucsi_i2c_ccg_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ucsi_i2c_ccg *ui = i2c_get_clientdata(client);
	int err;

	if (device_may_wakeup(dev)) {
		err = enable_irq_wake(ui->irq);
		if (!err)
			ui->wake_enabled = true;
	}
	return 0;
}

static int ucsi_i2c_ccg_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ucsi_i2c_ccg *ui = i2c_get_clientdata(client);
	struct ucsi_control c;
	int ret;

	if (device_may_wakeup(dev) && ui->wake_enabled) {
		disable_irq_wake(ui->irq);
		ui->wake_enabled = false;
	}

	/* restore UCSI notification enable mask */
	UCSI_CMD_SET_NTFY_ENABLE(c, UCSI_ENABLE_NTFY_ALL);
	ret = ucsi_run_command(ui->ucsi, &c, NULL, 0);
	if (ret) {
		dev_err(ui->dev, "%s: failed to set notification enable - %d\n",
			__func__, ret);
	}

	return 0;
}

UNIVERSAL_DEV_PM_OPS(ucsi_i2c_ccg_driver_pm, ucsi_i2c_ccg_suspend,
	ucsi_i2c_ccg_resume, NULL);

static const struct i2c_device_id ucsi_i2c_ccg_device_id[] = {
	{"i2c-gpu-ucsi", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, ucsi_i2c_ccg_device_id);

static struct i2c_driver ucsi_i2c_ccg_driver = {
	.driver = {
		.name = "ucsi_i2c_ccg",
		.pm = &ucsi_i2c_ccg_driver_pm,
	},
	.probe = ucsi_i2c_ccg_probe,
	.remove = ucsi_i2c_ccg_remove,
	.id_table = ucsi_i2c_ccg_device_id,
};

module_i2c_driver(ucsi_i2c_ccg_driver);

MODULE_AUTHOR("Ajay Gupta <ajayg@nvidia.com>");
MODULE_DESCRIPTION("UCSI I2C driver for Cypress CCGx Type-C controller");
MODULE_LICENSE("GPL v2");
