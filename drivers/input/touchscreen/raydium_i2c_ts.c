/*
 * Raydium touchscreen I2C driver.
 *
 * Copyright (C) 2012-2014, Raydium Semiconductor Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, and only version 2, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Raydium reserves the right to make changes without further notice
 * to the materials described herein. Raydium does not assume any
 * liability arising out of the application described herein.
 *
 * Contact Raydium Semiconductor Corporation at www.rad-ic.com
 */

#include <linux/module.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/input/mt.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/gpio.h>
#include <linux/regulator/consumer.h>
#include <asm/unaligned.h>
#include <linux/gpio/consumer.h>

/* Device, Driver information */
#define DEVICE_NAME	"raydium_i2c"

/* Slave I2C mode*/
#define RM_BOOT_BLDR	0x02
#define RM_BOOT_MAIN	0x03

/*I2C command */
#define CMD_QUERY_BANK		0x2B
#define CMD_DATA_BANK		0x4D
#define CMD_ENTER_SLEEP		0x4E
#define CMD_BOOT_ACK		0x0A
#define CMD_BOOT_WRT		0x5B
#define CMD_BOOT_CHK		0x0C
#define CMD_BANK_SWITCH		0xAA

/* Touch relative info */
#define MAX_RETRIES		3
#define MAX_FW_UPDATE_RETRIES	30
#define MAX_TOUCH_NUM		10
#define MAX_PACKET_SIZE		60
#define BOOT_DELAY_MS	100

#define RAYDIUM_FW_PAGESIZE	128
#define RAYDIUM_POWERON_DELAY_USEC	500
#define RAYDIUM_RESET_DELAY_MSEC	50

#define ADDR_INDEX		0x03
#define HEADER_SIZE		4

enum raydium_boot_mode {
	RAYDIUM_TS_MAIN,
	RAYDIUM_TS_BLDR,
};

struct raydium_info {
	u32 hw_ver;
	u8 main_ver;
	u8 sub_ver;
	u16 ft_ver;
	u8 x_num;
	u8 y_num;
	u16 x_max;
	u16 y_max;
	u8 x_res;		/* units/mm */
	u8 y_res;		/* units/mm */
};

struct raydium_abs_info {
	u8 state;/*1:touch, 0:no touch*/
	u8 x_pos_lsb;
	u8 x_pos_msb;
	u8 y_pos_lsb;
	u8 y_pos_msb;
	u8 pressure;
	u8 x_width;
	u8 y_width;
};

struct raydium_object {
	u32 data_bank_addr;
	u8 pkg_size;
};

/* struct raydium_data - represents state of Raydium touchscreen device */
struct raydium_data {
	struct i2c_client *client;
	struct input_dev *input;

	struct regulator *vcc33;
	struct regulator *vccio;
	struct gpio_desc *reset_gpio;

	u32 query_bank_info;

	struct raydium_info info;
	struct raydium_object obj;
	struct raydium_abs_info finger;

	enum raydium_boot_mode boot_mode;

	struct mutex sysfs_mutex;

	u8 cmd_resp[HEADER_SIZE];
	struct completion cmd_done;

	u8 buf[MAX_PACKET_SIZE];

	bool wake_irq_enabled;
};

static int raydium_i2c_send(struct i2c_client *client,
	u8 addr, u8 *data, size_t len)
{
	u8 buf[MAX_PACKET_SIZE + 1];
	int i, tries = 0;

	if (len > MAX_PACKET_SIZE)
		return -EINVAL;

	buf[0] = addr & 0xff;
	for (i = 0; i < len; i++)
		buf[i + 1] = *data++;

	do {
		if (i2c_master_send(client, buf, len + 1) == (len + 1))
			return 0;
		msleep(20);
	} while (++tries < MAX_RETRIES);

	dev_err(&client->dev, "%s: i2c send failed\n", __func__);

	return -EIO;
}

static int raydium_i2c_read(struct i2c_client *client,
	u8 addr, size_t len, void *data)
{
	struct i2c_msg xfer[2];

	/* Write register */
	xfer[0].addr = client->addr;
	xfer[0].flags = 0;
	xfer[0].len = 1;
	xfer[0].buf = &addr;

	/* Read data */
	xfer[1].addr = client->addr;
	xfer[1].flags = I2C_M_RD;
	xfer[1].len = len;
	xfer[1].buf = data;

	if (i2c_transfer(client->adapter, xfer, 2) == 2)
		return 0;

	dev_err(&client->dev, "%s: i2c transfer failed\n", __func__);

	return -EIO;
}

static int raydium_i2c_read_message(struct i2c_client *client,
	u32 addr, size_t len, void *data)
{
	u16 pkg_size, use_len;
	u8 buf[HEADER_SIZE], idx_i, idx_j;
	int error;

	use_len = len;
	idx_j = 0;
	while (use_len > 0) {
		pkg_size = (use_len < MAX_PACKET_SIZE) ?
			use_len : MAX_PACKET_SIZE;
		for (idx_i = 0; idx_i < HEADER_SIZE; idx_i++)
			buf[idx_i] = addr >> (3 - idx_i)*8;

		/*set data bank*/
		error = raydium_i2c_send(client, CMD_BANK_SWITCH,
			(u8 *)buf, HEADER_SIZE);
		/*read potints data*/
		if (!error)
			error = raydium_i2c_read(client,
				buf[ADDR_INDEX],
				pkg_size,
				(void *)(data + idx_j*MAX_PACKET_SIZE));

		pkg_size += MAX_PACKET_SIZE;
		addr += MAX_PACKET_SIZE;
		use_len = (use_len < MAX_PACKET_SIZE) ?
			0 : (use_len - MAX_PACKET_SIZE);
		idx_j++;
	}

	return error;
}

static int raydium_i2c_send_message(struct i2c_client *client,
	size_t len, void *data)
{
	int error;
	u8 buf[HEADER_SIZE], ii;

	for (ii = 0; ii < HEADER_SIZE; ii++)
		buf[ii] = ((u8 *)data)[3 - ii];
	/*set data bank*/
	error = raydium_i2c_send(client, CMD_BANK_SWITCH,
		(u8 *)buf, HEADER_SIZE);

	/*send messange*/
	if (!error)
		error = raydium_i2c_send(client, buf[ADDR_INDEX], buf, len);

	return error;
}

static int raydium_i2c_sw_reset(struct i2c_client *client)
{
	const u8 soft_rst_cmd[] = { 0x01, 0x04, 0x00, 0x00, 0x40};
	int error;

	error = raydium_i2c_send_message(client,
			1, (void *)soft_rst_cmd);
	if (error) {
		dev_err(&client->dev, "software reset failed: %d\n", error);
		return error;
	}

	msleep(RAYDIUM_RESET_DELAY_MSEC);

	return 0;
}

static int raydium_i2c_query_ts_info(struct raydium_data *ts)
{
	struct i2c_client *client = ts->client;
	int error, retry_cnt;

	for (retry_cnt = 0; retry_cnt < MAX_RETRIES; retry_cnt++) {
		error = raydium_i2c_read(client, CMD_DATA_BANK,
			sizeof(ts->obj), (void *)&ts->obj);
		if (error)
			dev_err(&client->dev, "get data bank failed: %d\n",
			error);
		else {
			error = raydium_i2c_read(client, CMD_QUERY_BANK,
				sizeof(ts->query_bank_info),
				(void *)&ts->query_bank_info);
			if (!error) {
				error = raydium_i2c_read_message(client,
					ts->query_bank_info,
					sizeof(ts->info),
					(void *)&ts->info);
			}
		}
		if (!error)
			return 0;
	}

	return -EINVAL;
}

static int raydium_i2c_fastboot(struct i2c_client *client)
{
	const u8 boot_cmd[] = { 0x20, 0x06, 0x00, 0x50 };
	u8 buf[HEADER_SIZE];
	int error;

	error = raydium_i2c_read_message(client,
		get_unaligned_be32(boot_cmd),
		sizeof(boot_cmd),
		buf);

	if (error) {
		dev_err(&client->dev, "boot failed: %d\n", error);
		return error;
	} else if (buf[0] == RM_BOOT_BLDR) {
		dev_dbg(&client->dev, "boot in fastboot mode\n");
		return -EINVAL;
	}

	dev_dbg(&client->dev, "boot success -- 0x%x\n", client->addr);
	return 0;
}

static int raydium_i2c_initialize(struct raydium_data *ts)
{
	struct i2c_client *client = ts->client;
	int error, retry_cnt;
	const u8 recov_packet[] = { 0x04, 0x81 };
	u8 buf[HEADER_SIZE];

	for (retry_cnt = 0; retry_cnt < MAX_RETRIES; retry_cnt++) {
		error = raydium_i2c_fastboot(client);
		if (error) {
			/* Continue initializing if it's the last try */
			if (retry_cnt < MAX_RETRIES - 1)
				continue;
		}
		/* Wait for Hello packet */
		msleep(BOOT_DELAY_MS);

		error = raydium_i2c_read(client, recov_packet[0],
			1, (void *)buf);
		if (error) {
			dev_err(&client->dev,
				"failed to read 'hello' packet: %d\n", error);
		} else if (buf[0] == recov_packet[1]) {
			ts->boot_mode = RAYDIUM_TS_MAIN;
			break;
		}
	}

	if (error)
		ts->boot_mode = RAYDIUM_TS_BLDR;
	else
		raydium_i2c_query_ts_info(ts);

	return error;
}

static int raydium_i2c_fw_write_page(struct i2c_client *client,
				    const void *page)
{
	const u8 ack_ok[] = { 0x55, 0xAA };
	u8 buf[2];
	int retry;
	int error;

	for (retry = 0; retry < MAX_FW_UPDATE_RETRIES; retry++) {
		error = raydium_i2c_send(client, CMD_BOOT_WRT,
			(u8 *)page, RAYDIUM_FW_PAGESIZE);
		if (error) {
			dev_err(&client->dev,
				"BLDR Write Page failed: %d\n", error);
			continue;
		}

		error = raydium_i2c_read(client, CMD_BOOT_CHK,
			sizeof(ack_ok), (void *)buf);
		if (error) {
			dev_err(&client->dev,
				"BLDR Ack read failed: %d\n", error);
			return error;
		}

		if (!memcmp(buf, ack_ok, sizeof(ack_ok)))
			return 0;

		error = -EIO;
		dev_err(&client->dev,
			"BLDR Get Ack Error [%02x:%02x]\n",
			buf[0], buf[1]);
	}

	return error;
}

static int raydium_i2c_do_update_firmware(struct i2c_client *client,
					 const struct firmware *fw,
					 bool force)
{
	const u8 boot_cmd[] = { RM_BOOT_BLDR, 0x20, 0x06, 0x00, 0x50 };
	const u8 main_cmd[] = { RM_BOOT_MAIN, 0x20, 0x06, 0x00, 0x50 };
	const u8 boot_ack[] = { 0x55, 0xAA, 0x00, 0xFF };
	u8 buf[HEADER_SIZE];
	int page, n_fw_pages;
	int error;

	/* Recovery mode detection! */
	if (!force) {
		/* Start boot loader Procedure */
		dev_dbg(&client->dev, "Normal BLDR procedure\n");
		/* switch to mode */
		error = raydium_i2c_send_message(client, 1, (void *)boot_cmd);
		if (error)
			dev_err(&client->dev, "failed to send boot cmd: %d\n",
				error);
		msleep(60);
		raydium_i2c_sw_reset(client);
		msleep(RAYDIUM_RESET_DELAY_MSEC);
		error = raydium_i2c_send_message(client, 1, (void *)boot_cmd);
	}

	if (error) {
		dev_err(&client->dev, "failed to enter fastboot mode: %d\n",
			error);
		return error;
	}

	msleep(20);

	/* check fastboot state */
	error = raydium_i2c_read(client, CMD_BOOT_ACK,
		sizeof(boot_ack), (void *)buf);
	if (error) {
		dev_err(&client->dev,
			"failed to read boot ack: %d\n",
			error);
		return error;
	}

	if (memcmp(buf, boot_ack, sizeof(boot_ack))) {
		dev_err(&client->dev,
			"failed to enter fastboot: %*ph (expected %*ph)\n",
			(int)sizeof(buf), buf, (int)sizeof(boot_ack), boot_ack);
		return -EIO;
	}

	dev_info(&client->dev, "successfully entered fastboot mode");

	n_fw_pages = fw->size / RAYDIUM_FW_PAGESIZE;
	dev_dbg(&client->dev, "BLDR Pages = %d\n", n_fw_pages);

	for (page = 0; page < n_fw_pages; page++) {
		error = raydium_i2c_fw_write_page(client,
					fw->data + page * RAYDIUM_FW_PAGESIZE);
		if (error) {
			dev_err(&client->dev,
				"failed to write FW page %d: %d\n",
				page, error);
			return error;
		}
	}
	error = raydium_i2c_send_message(client, 1, (void *)main_cmd);
	msleep(20);
	raydium_i2c_sw_reset(client);
	dev_info(&client->dev, "firmware update completed\n");

	return 0;
}

static int raydium_i2c_fw_update(struct raydium_data *ts)
{
	struct i2c_client *client = ts->client;
	const struct firmware *fw;
	char *fw_name;
	int error;

	fw_name = kasprintf(GFP_KERNEL, "raydium_i2c_%04x.bin",
		ts->info.hw_ver & 0xFFFF);
	if (!fw_name)
		return -ENOMEM;

	dev_info(&client->dev, "requesting fw name = %s\n", fw_name);
	error = request_firmware(&fw, fw_name, &client->dev);
	kfree(fw_name);
	if (error) {
		dev_err(&client->dev, "failed to request firmware: %d\n",
			error);
		return error;
	}

	if (fw->size % RAYDIUM_FW_PAGESIZE) {
		dev_err(&client->dev, "invalid firmware length: %zu\n",
			fw->size);
		error = -EINVAL;
		goto out;
	}

	disable_irq(client->irq);
	error = raydium_i2c_do_update_firmware(client, fw,
					ts->boot_mode == RAYDIUM_TS_BLDR);
	if (error) {
		dev_err(&client->dev, "firmware update failed: %d\n", error);
		ts->boot_mode = RAYDIUM_TS_BLDR;
		goto out_enable_irq;
	}
	error = raydium_i2c_initialize(ts);
	if (error) {
		dev_err(&client->dev,
			"failed to initialize device after firmware update: %d\n",
			error);
		ts->boot_mode = RAYDIUM_TS_BLDR;
		goto out_enable_irq;
	}

	ts->boot_mode = RAYDIUM_TS_MAIN;

out_enable_irq:
	enable_irq(client->irq);
	msleep(100);
out:
	release_firmware(fw);
	return error;
}

static void raydium_mt_event(struct raydium_data *ts)
{
	struct raydium_abs_info *data;
	int error, i, x, y;
	u8 f_state;
	u8 touch_count;
	u8 tp_info_size;

	error = raydium_i2c_read_message(ts->client, ts->obj.data_bank_addr,
		ts->obj.pkg_size, (void *)&ts->buf);

	if (error < 0) {
		dev_err(&ts->client->dev, "%s: failed to read data: %d\n",
			__func__, error);
		return;
	}

	touch_count = 0;
	tp_info_size = sizeof(ts->finger);

	for (i = 0; i < MAX_TOUCH_NUM; i++) {
		data = (struct raydium_abs_info *)(ts->buf + i * tp_info_size);

		f_state = data->state & 0x03;

		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input,
				MT_TOOL_FINGER, f_state != 0);

		if (!f_state)
			continue;

		x = (data->x_pos_msb << 8) | (data->x_pos_lsb);
		y = (data->y_pos_msb << 8) | (data->y_pos_lsb);

		input_report_key(ts->input,
			BTN_TOUCH, 1);
		input_report_key(ts->input,
			BTN_TOOL_FINGER, 1);
		input_report_abs(ts->input,
			ABS_MT_POSITION_X, x);
		input_report_abs(ts->input,
			ABS_MT_POSITION_Y, y);
		input_report_abs(ts->input,
			ABS_MT_PRESSURE, data->pressure);
		input_report_abs(ts->input,
			ABS_MT_TOUCH_MAJOR,
			max(data->x_width, data->y_width));
		input_report_abs(ts->input,
			ABS_MT_TOUCH_MINOR,
			min(data->x_width, data->y_width));
		touch_count++;
	}

	input_report_key(ts->input,
		BTN_TOUCH, touch_count > 0);
	input_report_key(ts->input,
		BTN_TOOL_FINGER, ts->input > 0);
	input_mt_report_pointer_emulation(ts->input, false);
	input_sync(ts->input);
}

static irqreturn_t raydium_i2c_irq(int irq, void *_dev)
{
	struct raydium_data *ts = _dev;

	if (ts->boot_mode != RAYDIUM_TS_BLDR)
		raydium_mt_event(ts);

	return IRQ_HANDLED;
}

static ssize_t write_update_fw(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t count)
{
	struct raydium_data *ts = dev_get_drvdata(dev);
	int error;

	error = mutex_lock_interruptible(&ts->sysfs_mutex);
	if (error)
		return error;

	error = raydium_i2c_fw_update(ts);
	dev_dbg(dev, "firmware update result: %d\n", error);

	mutex_unlock(&ts->sysfs_mutex);
	return error ?: count;
}

static ssize_t raydium_bootmode_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct raydium_data *ts = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n",
		ts->boot_mode == RAYDIUM_TS_MAIN ?
		"Normal" : "Recovery");
}

static ssize_t raydium_fw_ver_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct raydium_data *ts = dev_get_drvdata(dev);

	return sprintf(buf, "Release Version %d.%d\n",
		ts->info.main_ver,
		ts->info.sub_ver);
}

static ssize_t raydium_hw_ver_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct raydium_data *ts = dev_get_drvdata(dev);

	return sprintf(buf, "Hardware version 0x%x\n",
		ts->info.hw_ver & 0xFFFF);
}

static DEVICE_ATTR(fw_version, S_IRUGO, raydium_fw_ver_show, NULL);
static DEVICE_ATTR(hw_version, S_IRUGO, raydium_hw_ver_show, NULL);
static DEVICE_ATTR(boot_mode, S_IRUGO, raydium_bootmode_show, NULL);
static DEVICE_ATTR(update_fw, S_IWUSR, NULL, write_update_fw);

static struct attribute *raydium_attributes[] = {
	&dev_attr_update_fw.attr,
	&dev_attr_boot_mode.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_hw_version.attr,
	NULL
};

static struct attribute_group raydium_attribute_group = {
	.attrs = raydium_attributes,
};

static void raydium_i2c_remove_sysfs_group(void *_data)
{
	struct raydium_data *ts = _data;

	sysfs_remove_group(&ts->client->dev.kobj, &raydium_attribute_group);
}

static int raydium_i2c_power_on(struct raydium_data *ts)
{
	int error;

	if (IS_ERR_OR_NULL(ts->reset_gpio))
		return 0;

	gpiod_set_value_cansleep(ts->reset_gpio, 1);

	error = regulator_enable(ts->vcc33);
	if (error) {
		dev_err(&ts->client->dev,
			"failed to enable vcc33 regulator: %d\n",
			error);
		goto release_reset_gpio;
	}

	error = regulator_enable(ts->vccio);
	if (error) {
		dev_err(&ts->client->dev,
			"failed to enable vccio regulator: %d\n",
			error);
		regulator_disable(ts->vcc33);
		goto release_reset_gpio;
	}

	udelay(RAYDIUM_POWERON_DELAY_USEC);

release_reset_gpio:
	gpiod_set_value_cansleep(ts->reset_gpio, 0);

	if (error)
		return error;

	msleep(RAYDIUM_RESET_DELAY_MSEC);

	return 0;
}

static void raydium_i2c_power_off(void *_data)
{
	struct raydium_data *ts = _data;

	if (!IS_ERR_OR_NULL(ts->reset_gpio)) {
		gpiod_set_value_cansleep(ts->reset_gpio, 1);
		regulator_disable(ts->vccio);
		regulator_disable(ts->vcc33);
	}
}

static int raydium_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	union i2c_smbus_data dummy;
	struct raydium_data *ts;
	unsigned long irqflags;
	int error;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev,
			"%s: i2c check functionality error\n", DEVICE_NAME);
		return -ENXIO;
	}

	ts = devm_kzalloc(&client->dev,
		sizeof(struct raydium_data), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	mutex_init(&ts->sysfs_mutex);
	init_completion(&ts->cmd_done);

	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->vcc33 = devm_regulator_get(&client->dev, "avdd");
	if (IS_ERR(ts->vcc33)) {
		error = PTR_ERR(ts->vcc33);
		if (error != -EPROBE_DEFER)
			dev_err(&client->dev,
				"Failed to get 'vcc33' regulator: %d\n",
				error);
		return error;
	}

	ts->vccio = devm_regulator_get(&client->dev, "vdd");
	if (IS_ERR(ts->vccio)) {
		error = PTR_ERR(ts->vccio);
		if (error != -EPROBE_DEFER)
			dev_err(&client->dev,
				"Failed to get 'vccio' regulator: %d\n",
				error);
		return error;
	}


	ts->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
		GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset_gpio)) {
		error = PTR_ERR(ts->reset_gpio);
		if (error != -EPROBE_DEFER) {
			dev_err(&client->dev,
				"failed to get reset gpio: %d\n",
				error);
			return error;
		}
	}

	error = raydium_i2c_power_on(ts);
	if (error)
		return error;

	error = devm_add_action(&client->dev, raydium_i2c_power_off, ts);
	if (error) {
		dev_err(&client->dev,
			"failed to install power off action: %d\n", error);
		raydium_i2c_power_off(ts);
		return error;
	}

	/* Make sure there is something at this address */
	if (i2c_smbus_xfer(client->adapter, client->addr, 0,
			   I2C_SMBUS_READ, 0, I2C_SMBUS_BYTE, &dummy) < 0) {
		dev_err(&client->dev, "nothing at this address\n");
		return -ENXIO;
	}

	error = raydium_i2c_initialize(ts);
	if (error) {
		dev_err(&client->dev, "failed to initialize: %d\n", error);
		return error;
	}

	ts->input = devm_input_allocate_device(&client->dev);
	if (!ts->input) {
		dev_err(&client->dev, "Failed to allocate input device\n");
		return -ENOMEM;
	}

	ts->input->name = "Raydium Touchscreen";
	ts->input->id.bustype = BUS_I2C;

	__set_bit(BTN_TOUCH, ts->input->keybit);
	__set_bit(EV_ABS, ts->input->evbit);
	__set_bit(EV_KEY, ts->input->evbit);

	/* Single touch input params setup */
	input_set_abs_params(ts->input, ABS_X, 0, ts->info.x_max, 0, 0);
	input_set_abs_params(ts->input, ABS_Y, 0, ts->info.y_max, 0, 0);
	input_set_abs_params(ts->input, ABS_PRESSURE, 0, 255, 0, 0);
	input_abs_set_res(ts->input, ABS_X, ts->info.x_res);
	input_abs_set_res(ts->input, ABS_Y, ts->info.y_res);

	/* Multitouch input params setup */
	error = input_mt_init_slots(ts->input, MAX_TOUCH_NUM,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error) {
		dev_err(&client->dev,
			"failed to initialize MT slots: %d\n", error);
		return error;
	}

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0,
		ts->info.x_max, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y,
		0, ts->info.y_max, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_abs_set_res(ts->input, ABS_MT_POSITION_X, ts->info.x_res);
	input_abs_set_res(ts->input, ABS_MT_POSITION_Y, ts->info.y_res);

	input_mt_init_slots(ts->input, MAX_TOUCH_NUM, 0);

	input_set_drvdata(ts->input, ts);

	error = input_register_device(ts->input);
	if (error) {
		dev_err(&client->dev,
			"unable to register input device: %d\n", error);
		return error;
	}

	error = devm_request_threaded_irq(&client->dev, client->irq,
					  NULL, raydium_i2c_irq,
					  irqflags | IRQF_ONESHOT,
					  client->name, ts);
	if (error) {
		dev_err(&client->dev, "Failed to register interrupt\n");
		return error;
	}

	if (!client->dev.of_node)
		device_init_wakeup(&client->dev, true);

	error = sysfs_create_group(&client->dev.kobj, &raydium_attribute_group);
	if (error) {
		dev_err(&client->dev, "failed to create sysfs attributes: %d\n",
			error);
		return error;
	}

	error = devm_add_action(&client->dev,
				raydium_i2c_remove_sysfs_group, ts);
	if (error) {
		raydium_i2c_remove_sysfs_group(ts);
		dev_err(&client->dev,
			"Failed to add sysfs cleanup action: %d\n",
			error);
		return error;
	}

	return 0;
}

static int raydium_i2c_remove(struct i2c_client *client)
{
	struct raydium_data *ts = i2c_get_clientdata(client);

	if (ts->input)
		input_unregister_device(ts->input);

	device_init_wakeup(&client->dev, false);

	devm_free_irq(&client->dev, client->irq, ts);

	mutex_destroy(&ts->sysfs_mutex);

	devm_remove_action(&client->dev, raydium_i2c_remove_sysfs_group, ts);

	devm_remove_action(&client->dev, raydium_i2c_power_off, ts);

	return 0;
}

static void __maybe_unused raydium_enter_sleep(struct i2c_client *client)
{
	const u8 sleep_cmd[] = { 0x5A, 0xff, 0x00, 0x0f };
	int error;

	error = raydium_i2c_send(client, CMD_ENTER_SLEEP,
			(u8 *)sleep_cmd, sizeof(sleep_cmd));
	if (error)
		dev_err(&client->dev,
			"Send sleep failed: %d\n", error);
}

static int __maybe_unused raydium_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct raydium_data *ts = i2c_get_clientdata(client);

	/* Command not support in BLDR recovery mode */
	if (ts->boot_mode != RAYDIUM_TS_MAIN)
		return -EBUSY;

	disable_irq(client->irq);

	if (device_may_wakeup(dev)) {
		raydium_enter_sleep(client);

		ts->wake_irq_enabled = (enable_irq_wake(client->irq) == 0);
	} else {
		raydium_i2c_power_off(ts);
	}

	return 0;
}

static int __maybe_unused raydium_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct raydium_data *ts = i2c_get_clientdata(client);

	if (device_may_wakeup(dev)) {
		if (ts->wake_irq_enabled)
			disable_irq_wake(client->irq);
		raydium_i2c_sw_reset(client);
	} else {
		raydium_i2c_power_on(ts);
		raydium_i2c_initialize(ts);
	}

	enable_irq(client->irq);

	return 0;
}

static SIMPLE_DEV_PM_OPS(raydium_i2c_pm_ops,
			 raydium_i2c_suspend, raydium_i2c_resume);

static const struct i2c_device_id raydium_i2c_id[] = {
	{ DEVICE_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, raydium_i2c_id);

#ifdef CONFIG_ACPI
static const struct acpi_device_id raydium_acpi_id[] = {
	{ "RMTS_0001", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, raydium_acpi_id);
#endif

#ifdef CONFIG_OF
static const struct of_device_id raydium_of_match[] = {
	{ .compatible = "raydium,rm-ts",},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, raydium_of_match);
#endif

static struct i2c_driver raydium_i2c_driver = {
	.probe = raydium_i2c_probe,
	.remove = raydium_i2c_remove,
	.id_table = raydium_i2c_id,
	.driver = {
		.name = "raydium_ts",
		.pm = &raydium_i2c_pm_ops,
		.acpi_match_table = ACPI_PTR(raydium_acpi_id),
		.of_match_table = of_match_ptr(raydium_of_match),
	},
};

module_i2c_driver(raydium_i2c_driver);

MODULE_AUTHOR("Raydium");
MODULE_DESCRIPTION("Raydium I2c Touchscreen driver");
MODULE_LICENSE("GPL");
