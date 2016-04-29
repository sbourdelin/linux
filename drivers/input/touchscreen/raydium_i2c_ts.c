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
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/input/mt.h>
#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <asm/unaligned.h>
#include <linux/gpio/consumer.h>

/* Device, Driver information */
#define DEVICE_NAME	"raydium_i2c"

/* Slave I2C mode*/
#define RM_BOOT_BLDR	0x02
#define RM_BOOT_MAIN	0x03

/*I2C bl command */
#define CMD_BOOT_PAGE_WRT	0x0B		/*send bl page write*/
#define CMD_BOOT_WRT		0x11		/*send bl write*/
#define CMD_BOOT_ACK		0x22		/*send ack*/
#define CMD_BOOT_CHK		0x33		/*send data check*/
#define CMD_BOOT_READ		0x44		/*send wait bl data ready*/
#define BOOT_RDY		0xFF		/*bl data ready*/
/*I2C main command*/
#define CMD_QUERY_BANK		0x2B
#define CMD_DATA_BANK		0x4D
#define CMD_ENTER_SLEEP		0x4E
#define CMD_BANK_SWITCH		0xAA

/* Touch relative info */
#define MAX_RETRIES		3
#define MAX_TOUCH_NUM		10
#define MAX_PkG_SIZE		50
#define BOOT_DELAY_MS	100

/*Bootloader relative info */
#define CMD_BOOT_HEADER_LEN		3	/*bl flash wrt cmd size*/
#define RAYDIUM_TRANS_BUFSIZE	32	/*bl wrt pkg size*/
#define MAX_BOOT_WRT_LEN	(RAYDIUM_TRANS_BUFSIZE + CMD_BOOT_HEADER_LEN)
#define MAX_FW_UPDATE_RETRIES	30

enum raydium_bl_cmd {
	HEADER = 0,
	PAGE_STR,
	PKG_IDX,
	DATA_STR,
};

enum raydium_bl_ack {
	ACK_NULL = 0,
	WAIT_READY,
	PATH_READY,
};

#define RAYDIUM_PAGE_SIZE		128
#define RAYDIUM_POWERON_DELAY_USEC	500
#define RAYDIUM_RESET_DELAY_MSEC	50

#define ADDR_INDEX		0x03
#define DATA_INDEX		0x04

#define HEADER_SIZE		4

enum raydium_boot_mode {
	RAYDIUM_TS_MAIN = 0,
	RAYDIUM_TS_BLDR,
};

enum raydium_abs_idx {
	POS_STATE = 0,/*1:touch, 0:no touch*/
	POS_X,
	POS_Y = 3,
	POS_PRESSURE,
	WIDTH_X,
	WIDTH_Y,
};

struct raydium_info {
	u32 hw_ver;	/*device ver, __le32*/
	u8 main_ver;
	u8 sub_ver;
	u16 ft_ver;	/*test ver, __le16*/
	u8 x_num;
	u8 y_num;
	u16 x_max;	/*disp reso, __le16*/
	u16 y_max;	/*disp reso, __le16*/
	u8 x_res;		/* units/mm */
	u8 y_res;		/* units/mm */
};

struct raydium_object {
	u32 data_bank_addr;
	u8 pkg_size;
	u8 tp_info_size;
};

/* struct raydium_data - represents state of Raydium touchscreen device */
struct raydium_data {
	struct i2c_client *client;
	struct input_dev *input;

	struct regulator *avdd;
	struct regulator *vccio;
	struct gpio_desc *reset_gpio;

	u32 query_bank_info;

	struct raydium_info info;
	struct raydium_object obj;
	enum raydium_boot_mode boot_mode;

	struct mutex sysfs_mutex;
	struct completion cmd_done;

	bool wake_irq_enabled;
};

static int raydium_i2c_send(struct i2c_client *client,
	u8 addr, u8 *data, size_t len)
{
	u8 buf[MAX_PkG_SIZE + 1];
	int tries = 0;

	buf[0] = addr;
	memcpy(&buf[1], data, len);

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
	int ret;

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

	ret = i2c_transfer(client->adapter, xfer, ARRAY_SIZE(xfer));
	if (ret < 0)
		return ret;

	if (ret != ARRAY_SIZE(xfer))
		return -EIO;

	return 0;
}

static int raydium_i2c_read_message(struct i2c_client *client,
	u32 addr, size_t len, void *data)
{
	u8 buf[HEADER_SIZE];
	u8 read_cnt, idx_i;
	int error;
	size_t pkg_size;

	if (len % MAX_PkG_SIZE)
		read_cnt = len / MAX_PkG_SIZE + 1;
	else
		read_cnt = len / MAX_PkG_SIZE;

	idx_i = 0;
	for (idx_i = 0; idx_i < read_cnt; idx_i++) {
		pkg_size = (len > MAX_PkG_SIZE) ? MAX_PkG_SIZE : len;

		len -= MAX_PkG_SIZE;

		put_unaligned_be32(addr, buf);

		/*set data bank*/
		error = raydium_i2c_send(client, CMD_BANK_SWITCH,
			(u8 *)buf, HEADER_SIZE);
		/*read potints data*/
		if (!error)
			error = raydium_i2c_read(client, buf[ADDR_INDEX],
				pkg_size,
				data + idx_i*MAX_PkG_SIZE);

		addr += MAX_PkG_SIZE;
		idx_i++;
	}

	return error;
}

static int raydium_i2c_send_message(struct i2c_client *client,
	size_t len, void *data)
{
	int error;
	__le32 cmd;

	cmd = get_unaligned_le32(data);

	/*set data bank*/
	error = raydium_i2c_send(client, CMD_BANK_SWITCH, (u8 *)&cmd,
		HEADER_SIZE);

	/*send message*/
	if (!error)
		error = raydium_i2c_send(client, ((u8 *)data)[ADDR_INDEX],
			data + DATA_INDEX, len);

	return error;
}

static int raydium_i2c_sw_reset(struct i2c_client *client)
{
	static const u8 soft_rst_cmd[] = {0x40, 0x00, 0x00, 0x04, 0x01};
	int error;

	error = raydium_i2c_send_message(client, ARRAY_SIZE(soft_rst_cmd),
		(void *)soft_rst_cmd);
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
			ts->obj.data_bank_addr =
				get_unaligned_le32(&ts->obj.data_bank_addr);

		if (!error) {
			error = raydium_i2c_read(client, CMD_QUERY_BANK,
				sizeof(ts->query_bank_info),
				(void *)&ts->query_bank_info);
			if (!error) {
				error = raydium_i2c_read_message(client,
					ts->query_bank_info, sizeof(ts->info),
					(void *)&ts->info);

				ts->info.hw_ver =
					get_unaligned_le32(&ts->info.hw_ver);
				ts->info.ft_ver =
					get_unaligned_le16(&ts->info.ft_ver);
				ts->info.x_max =
					get_unaligned_le16(&ts->info.x_max);
				ts->info.y_max =
					get_unaligned_le16(&ts->info.y_max);
				return 0;
			}
		}
	}
	dev_err(&client->dev, "Get touch data failed: %d\n", error);

	return -EINVAL;
}

static int raydium_i2c_fastboot(struct i2c_client *client)
{
	static const u8 boot_cmd[] = { 0x50, 0x00, 0x06, 0x20 };
	u8 buf[HEADER_SIZE];
	int error;

	error = raydium_i2c_read_message(client,
		get_unaligned_be32(boot_cmd),
		sizeof(boot_cmd), buf);

	if (!error) {
		if (buf[0] == RM_BOOT_BLDR) {
			dev_dbg(&client->dev, "boot in fastboot mode\n");
			return -EINVAL;
		}
		dev_dbg(&client->dev, "boot success -- 0x%x\n", client->addr);
		return 0;
	}

	dev_err(&client->dev, "boot failed: %d\n", error);

	return error;
}

static int raydium_i2c_check_fw_status(struct raydium_data *ts)
{
	struct i2c_client *client = ts->client;
	static const u8 bl_area[] = {0x62, 0x6f, 0x6f, 0x74};
	static const u8 main_area[] = {0x66, 0x69, 0x72, 0x6d};
	u8 buf[HEADER_SIZE];
	int error;

	error = raydium_i2c_read(client, CMD_BOOT_READ, HEADER_SIZE,
		(void *)buf);
	if (!error) {
		if (buf[0] == bl_area[0])
			ts->boot_mode = RAYDIUM_TS_BLDR;
		else if (buf[0] == main_area[0])
			ts->boot_mode = RAYDIUM_TS_MAIN;
		return 0;
	}

	return error;
}

static int raydium_i2c_initialize(struct raydium_data *ts)
{
	struct i2c_client *client = ts->client;
	int error, retry_cnt;

	for (retry_cnt = 0; retry_cnt < MAX_RETRIES; retry_cnt++) {
		error = raydium_i2c_fastboot(client);
		if (error) {
			/* Continue initializing if it's the last try */
			if (retry_cnt < MAX_RETRIES - 1)
				continue;
		}
		/* Wait for Hello packet */
		msleep(BOOT_DELAY_MS);

		error = raydium_i2c_check_fw_status(ts);
		if (ts->boot_mode == RAYDIUM_TS_BLDR ||
			ts->boot_mode == RAYDIUM_TS_MAIN)
			break;
		else if (error) {
			dev_err(&client->dev,
				"failed to read 'hello' packet: %d\n", error);
		}
	}

	if (error)
		ts->boot_mode = RAYDIUM_TS_BLDR;

	if (ts->boot_mode == RAYDIUM_TS_BLDR) {
		ts->info.hw_ver = 0xffffffff;
		ts->info.main_ver = 0xff;
		ts->info.sub_ver = 0xff;
	} else {
		raydium_i2c_query_ts_info(ts);
	}

	return error;
}

static int raydium_i2c_bl_chk_state(struct i2c_client *client,
		enum raydium_bl_ack state)
{
	static const u8 ack_ok[] = { 0xFF, 0x39, 0x30, 0x30, 0x54 };
	u8 rbuf[5];
	u8 retry;
	int error;

	if (state == ACK_NULL)
		return 0;

	for (retry = 0; retry < MAX_FW_UPDATE_RETRIES; retry++) {
		if (state == WAIT_READY) {
			error = raydium_i2c_read(client, CMD_BOOT_CHK,
				1, &rbuf[0]);
			if (!error) {
				if (rbuf[0] == BOOT_RDY)
					return 0;
			}
		} else if (state == PATH_READY) {
			error = raydium_i2c_read(client, CMD_BOOT_CHK,
				sizeof(ack_ok), &rbuf[0]);
			if (!error) {
				if (!memcmp(rbuf, ack_ok, sizeof(ack_ok)))
					return 0;
			}
		} else
			return -EINVAL;
		msleep(20);
	}

	return -EINVAL;
}

static int raydium_i2c_wrt_object(struct i2c_client *client,
	u8 *data, size_t len, enum raydium_bl_ack state)
{
	int error = 0;

	error = raydium_i2c_send(client, CMD_BOOT_WRT, data, len);
	if (error) {
		dev_err(&client->dev, "WRT obj command failed: %d\n",
			error);
		return error;
	}

	error = raydium_i2c_send(client, CMD_BOOT_ACK, (u8 *)NULL, 0);
	if (error) {
		dev_err(&client->dev, "Ack obj command failed: %d\n", error);
		return error;
	}

	error = raydium_i2c_bl_chk_state(client, state);
	if (error) {
		dev_err(&client->dev, "boot trigger state failed: %d\n", error);
		return error;
	}

	return 0;
}

static bool raydium_i2c_boot_trigger(struct i2c_client *client)
{
	int error;
	u8 u8_idx;
	static const u8 cmd[7][6] = {
			{0x08, 0x0C, 0x09, 0x00, 0x50, 0xD7},
			{0x08, 0x04, 0x09, 0x00, 0x50, 0xA5},
			{0x08, 0x04, 0x09, 0x00, 0x50, 0x00},
			{0x08, 0x04, 0x09, 0x00, 0x50, 0xA5},
			{0x08, 0x0C, 0x09, 0x00, 0x50, 0x00},
			{0x06, 0x01, 0x00, 0x00, 0x00, 0x00},
			{0x02, 0xA2, 0x00, 0x00, 0x00, 0x00},
		};

	/*sequtial cmd*/
	for (u8_idx = 0 ; u8_idx < 7 ; u8_idx++) {
		error = raydium_i2c_wrt_object(client, (u8 *)cmd[u8_idx],
				sizeof(cmd[u8_idx]), WAIT_READY);
		if (error) {
			dev_err(&client->dev, "send boot trigger 1st_cmd failed: %d\n",
				error);
			return error;
		}
	}
	return 0;
}

static bool raydium_i2c_fw_trigger(struct i2c_client *client)
{
	int error;
	u8 u8_idx;
	static const u8 cmd[5][11] = {
			{0, 0x09, 0x71, 0x0C, 0x09, 0x00, 0x50, 0xD7, 0, 0, 0},
			{0, 0x09, 0x71, 0x04, 0x09, 0x00, 0x50, 0xA5, 0, 0, 0},
			{0, 0x09, 0x71, 0x04, 0x09, 0x00, 0x50, 0x00, 0, 0, 0},
			{0, 0x09, 0x71, 0x04, 0x09, 0x00, 0x50, 0xA5, 0, 0, 0},
			{0, 0x09, 0x71, 0x0C, 0x09, 0x00, 0x50, 0x00, 0, 0, 0},
		};

	/*sequtial cmd*/
	for (u8_idx = 0 ; u8_idx < 5 ; u8_idx++) {
		error = raydium_i2c_wrt_object(client, (u8 *)cmd[u8_idx],
				sizeof(cmd[u8_idx]), ACK_NULL);
		if (error) {
			dev_err(&client->dev, "send fw trigger 1st_cmd failed: %d\n",
				error);
			return error;
		}
	}
	return 0;
}

static int raydium_i2c_check_path(struct i2c_client *client)
{
	static const u8 cmd[] = {0x09, 0x00, 0x09, 0x00, 0x50, 0x10, 0x00};
	int error;

	error = raydium_i2c_wrt_object(client, (u8 *)cmd, sizeof(cmd),
			PATH_READY);
	if (error) {
		dev_err(&client->dev, "send chk path cmd fail: %d\n", error);
		return error;
	}

	return error;
}

static int raydium_i2c_enter_bl(struct i2c_client *client)
{
	static const u8 cal_cmd[] = {0x00, 0x01, 0x52};
	int error;

	error = raydium_i2c_wrt_object(client, (u8 *)cal_cmd,
			sizeof(cal_cmd), ACK_NULL);
	if (error) {
		dev_err(&client->dev, "send jump loader cmd fail: %d\n", error);
		return error;
	}
	msleep(BOOT_DELAY_MS);
	return 0;
}

static int raydium_i2c_leave_bl(struct i2c_client *client)
{
	static const u8 leave_cmd[] = {0x05, 0x00};
	int error;

	error = raydium_i2c_wrt_object(client, (u8 *)leave_cmd,
			sizeof(leave_cmd), ACK_NULL);
	if (error) {
		dev_err(&client->dev, "send leave bl cmd fail: %d\n", error);
		return error;
	}
	msleep(BOOT_DELAY_MS);
	return 0;
}

static int raydium_i2c_write_checksum(struct i2c_client *client,
	size_t length, u16 checksum)
{
	u8 checksum_cmd[] = {0x00, 0x05, 0x6D, 0x00, 0x00, 0x00, 0x00};
	int error = 0;

	checksum_cmd[3] = (u8)(length & 0xFF);
	checksum_cmd[4] = (u8)((length & 0xFF00) >> 8);
	checksum_cmd[5] = (u8)(checksum & 0xFF);
	checksum_cmd[6] = (u8)((checksum & 0xFF00) >> 8);

	error = raydium_i2c_wrt_object(client, (u8 *)checksum_cmd,
			sizeof(checksum_cmd), ACK_NULL);
	if (error) {
		dev_err(&client->dev, "send wrt checksum cmd fail: %d\n",
			error);
		return error;
	}

	return 0;
}

static int raydium_i2c_disable_watch_dog(struct i2c_client *client)
{
	static const u8 cmd[] = {0x0A, 0xAA};
	int error = 0;

	error = raydium_i2c_wrt_object(client, (u8 *)cmd, sizeof(cmd),
			WAIT_READY);
	if (error) {
		dev_err(&client->dev, "send disable watchdog cmd fail: %d\n",
			error);
		return error;
	}

	return 0;
}

static int raydium_i2c_fw_write_page(struct i2c_client *client,
				u8 *page, size_t len)
{
	int error;
	u8 buf[MAX_BOOT_WRT_LEN];
	u8 u8_idx, div_cnt;

	len -= CMD_BOOT_HEADER_LEN;

	div_cnt = len % RAYDIUM_TRANS_BUFSIZE ?
		len / RAYDIUM_TRANS_BUFSIZE + 1:len / RAYDIUM_TRANS_BUFSIZE;

	for (u8_idx = 0 ; u8_idx < div_cnt ; u8_idx++) {
		buf[HEADER] = page[0];
		buf[PAGE_STR] = page[1];
		buf[PKG_IDX] = u8_idx + 1;

		memcpy(&buf[DATA_STR], page + DATA_STR +
			u8_idx*RAYDIUM_TRANS_BUFSIZE,
			RAYDIUM_TRANS_BUFSIZE);

		error = raydium_i2c_wrt_object(client, (u8 *)buf, sizeof(buf),
				WAIT_READY);
		if (error) {
			dev_err(&client->dev, "send page wrt cmd failed: %d\n",
				error);
			return error;
		}
		msleep(20);
	}

	return error;
}

static int raydium_i2c_do_update_firmware(struct raydium_data *ts,
					 const struct firmware *fw)
{
	struct i2c_client *client = ts->client;
	u8 u8_idx;
	u16 fw_checksum;
	u8 buf[RAYDIUM_PAGE_SIZE + CMD_BOOT_HEADER_LEN];
	int page, n_fw_pages;
	int error, fw_idx;

	if (fw->size == 0) {
		dev_err(&client->dev, "Invalid firmware length\n");
		return -EINVAL;
	}

	if (fw->size % RAYDIUM_PAGE_SIZE)
		n_fw_pages = fw->size/RAYDIUM_PAGE_SIZE + 1;
	else
		n_fw_pages = fw->size/RAYDIUM_PAGE_SIZE;

	error = raydium_i2c_check_fw_status(ts);
	if (error) {
		dev_err(&client->dev, "Unable to access IC %d\n", error);
			return error;
	}

	if (ts->boot_mode == RAYDIUM_TS_MAIN) {
		for (u8_idx = 0; u8_idx < MAX_RETRIES; u8_idx++) {
			error = raydium_i2c_enter_bl(client);
			if (!error) {
				error = raydium_i2c_check_fw_status(ts);
				if (error) {
					dev_err(&client->dev, "Unable to access IC %d\n",
						error);
					return error;
				}
				if (ts->boot_mode == RAYDIUM_TS_BLDR)
					break;
			}
		}
		if (ts->boot_mode == RAYDIUM_TS_MAIN) {
			dev_err(&client->dev, "Fail jump to boot loader %d\n",
					error);
			return -EIO;
		}
	}

	error = raydium_i2c_disable_watch_dog(client);
	if (error) {
		dev_err(&client->dev, "send disable watchdog cmd fail, %d\n",
			error);
		return error;
	}

	error = raydium_i2c_check_path(client);
	if (error) {
		dev_err(&client->dev, "send chk path fail, %d\n", error);
		return error;
	}

	error = raydium_i2c_boot_trigger(client);
	if (error) {
		dev_err(&client->dev, "send boot trigger fail, %d\n", error);
		return error;
	}

	fw_checksum = 0;
	fw_idx = 0;
	for (page = 0 ; page < n_fw_pages ; page++) {
		memset(buf, 0xFF, sizeof(buf));
		buf[HEADER] = 0x0B;
		if (page == 0)
			buf[PAGE_STR] = 0x00;

		for (u8_idx = 0; u8_idx < RAYDIUM_PAGE_SIZE; u8_idx++) {
			if (fw_idx < fw->size) {
				buf[DATA_STR + u8_idx] =
					fw->data[page*RAYDIUM_PAGE_SIZE +
					u8_idx];
				fw_checksum += buf[3 + u8_idx];
				fw_idx++;
			}
		}

		raydium_i2c_fw_write_page(client, (u8 *)buf, sizeof(buf));
		msleep(20);
	}

	error = raydium_i2c_leave_bl(client);
	if (error) {
		dev_err(&client->dev, "leave boot loader fail: %d\n", error);
		return error;
	}
	dev_err(&client->dev, "leave boot loader success\n");

	error = raydium_i2c_check_fw_status(ts);
	if (error) {
		dev_err(&client->dev, "Unable to access IC %d\n", error);
		return error;
	}

	if (ts->boot_mode == RAYDIUM_TS_MAIN) {

		error = raydium_i2c_fw_trigger(client);
		if (error) {
			dev_err(&client->dev, "send fw trigger fail, %d\n",
				error);
			return error;
		}

		error = raydium_i2c_write_checksum(client, fw->size,
			fw_checksum);
		if (error) {
			dev_err(&client->dev, "write checksum fail %d\n",
				error);
			return error;
		}
	} else {
		dev_err(&client->dev, "switch to main_fw fail %d\n", error);
		return -EINVAL;
	}

	return 0;
}

static int raydium_i2c_fw_update(struct raydium_data *ts)
{
	struct i2c_client *client = ts->client;
	const struct firmware *fw = NULL;
	const char *fw_file = "raydium.fw";
	int error;

	error = request_firmware(&fw, fw_file, &client->dev);
	if (error) {
		dev_err(&client->dev, "Unable to open firmware %s\n", fw_file);
		return error;
	}
	/*disable irq*/
	disable_irq(client->irq);

	error = raydium_i2c_do_update_firmware(ts, fw);
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

	return error;
}

static void raydium_mt_event(struct raydium_data *ts)
{
	u8 data[MAX_PkG_SIZE];
	int error, i;
	int x, y, f_state, pressure, wx, wy;

	error = raydium_i2c_read_message(ts->client, ts->obj.data_bank_addr,
		ts->obj.pkg_size, (void *)data);

	if (error < 0) {
		dev_err(&ts->client->dev, "%s: failed to read data: %d\n",
			__func__, error);
		return;
	}

	for (i = 0; i < MAX_TOUCH_NUM; i++) {
		f_state = (data + i * ts->obj.tp_info_size)[POS_STATE];
		pressure = (data + i * ts->obj.tp_info_size)[POS_PRESSURE];
		wx = (data + i * ts->obj.tp_info_size)[WIDTH_X];
		wy = (data + i * ts->obj.tp_info_size)[WIDTH_Y];

		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input,
				MT_TOOL_FINGER, f_state != 0);

		if (!f_state)
			continue;
		x = get_unaligned_le16(&data[i * ts->obj.tp_info_size + POS_X]);
		y = get_unaligned_le16(&data[i * ts->obj.tp_info_size + POS_Y]);

		input_report_abs(ts->input, ABS_MT_POSITION_X, x);
		input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
		input_report_abs(ts->input, ABS_MT_PRESSURE, pressure);
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR,
			max(wx, wy));
		input_report_abs(ts->input, ABS_MT_TOUCH_MINOR,
			min(wx, wy));
	}

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static irqreturn_t raydium_i2c_irq(int irq, void *_dev)
{
	struct raydium_data *ts = _dev;

	if (ts->boot_mode != RAYDIUM_TS_BLDR)
		raydium_mt_event(ts);

	return IRQ_HANDLED;
}

static ssize_t raydium_calibrate(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct raydium_data *ts = dev_get_drvdata(dev);
	struct i2c_client *client = ts->client;

	static const u8 cal_cmd[] = {0x00, 0x01, 0x9E};
	int error = 0;

	error = raydium_i2c_wrt_object(client, (u8 *)cal_cmd,
			sizeof(cal_cmd), WAIT_READY);
	if (error) {
		dev_err(&client->dev, "send chk path cmd fail: %d\n", error);
		return error;
	}

	return error;
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

	return sprintf(buf, "%s\n", ts->boot_mode == RAYDIUM_TS_MAIN ?
		"Normal" : "Recovery");
}

static ssize_t raydium_fw_ver_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct raydium_data *ts = dev_get_drvdata(dev);

	return sprintf(buf, "%d.%d\n", ts->info.main_ver, ts->info.sub_ver);
}

static ssize_t raydium_hw_ver_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct raydium_data *ts = dev_get_drvdata(dev);

	return sprintf(buf, "%04x\n", ts->info.hw_ver);
}

static DEVICE_ATTR(fw_version, S_IRUGO, raydium_fw_ver_show, NULL);
static DEVICE_ATTR(hw_version, S_IRUGO, raydium_hw_ver_show, NULL);
static DEVICE_ATTR(boot_mode, S_IRUGO, raydium_bootmode_show, NULL);
static DEVICE_ATTR(update_fw, S_IWUSR, NULL, write_update_fw);
static DEVICE_ATTR(calibrate, S_IWUSR, NULL, raydium_calibrate);

static struct attribute *raydium_attributes[] = {
	&dev_attr_update_fw.attr,
	&dev_attr_boot_mode.attr,
	&dev_attr_fw_version.attr,
	&dev_attr_hw_version.attr,
	&dev_attr_calibrate.attr,
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

	error = regulator_enable(ts->avdd);
	if (error) {
		dev_err(&ts->client->dev,
			"failed to enable avdd regulator: %d\n", error);
		goto release_reset_gpio;
	}

	error = regulator_enable(ts->vccio);
	if (error) {
		regulator_disable(ts->avdd);
		dev_err(&ts->client->dev,
			"failed to enable vccio regulator: %d\n", error);
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
		regulator_disable(ts->avdd);
	}
}

static int raydium_i2c_probe(struct i2c_client *client,
			    const struct i2c_device_id *id)
{
	union i2c_smbus_data dummy;
	struct raydium_data *ts;
	int error;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev,
			"%s: i2c check functionality error\n", DEVICE_NAME);
		return -ENXIO;
	}

	ts = devm_kzalloc(&client->dev, sizeof(struct raydium_data),
		GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	mutex_init(&ts->sysfs_mutex);
	init_completion(&ts->cmd_done);

	ts->client = client;
	i2c_set_clientdata(client, ts);

	ts->avdd = devm_regulator_get(&client->dev, "avdd");
	if (IS_ERR(ts->avdd)) {
		error = PTR_ERR(ts->avdd);
		if (error != -EPROBE_DEFER)
			dev_err(&client->dev,
				"Failed to get 'avdd' regulator: %d\n", error);
		return error;
	}

	ts->vccio = devm_regulator_get(&client->dev, "vccio");
	if (IS_ERR(ts->vccio)) {
		error = PTR_ERR(ts->vccio);
		if (error != -EPROBE_DEFER)
			dev_err(&client->dev,
				"Failed to get 'vccio' regulator: %d\n", error);
		return error;
	}

	ts->reset_gpio = devm_gpiod_get_optional(&client->dev, "reset",
		GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset_gpio)) {
		error = PTR_ERR(ts->reset_gpio);
		if (error != -EPROBE_DEFER) {
			dev_err(&client->dev,
				"failed to get reset gpio: %d\n", error);
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

	/* Multitouch input params setup */
	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0,
		ts->info.x_max, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y,
		0, ts->info.y_max, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_abs_set_res(ts->input, ABS_MT_POSITION_X, ts->info.x_res);
	input_abs_set_res(ts->input, ABS_MT_POSITION_Y, ts->info.y_res);

	input_set_drvdata(ts->input, ts);

	error = input_mt_init_slots(ts->input, MAX_TOUCH_NUM,
		INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error) {
		dev_err(&client->dev,
			"failed to initialize MT slots: %d\n", error);
		return error;
	}

	error = input_register_device(ts->input);
	if (error) {
		dev_err(&client->dev,
			"unable to register input device: %d\n", error);
		return error;
	}

	error = devm_request_threaded_irq(&client->dev, client->irq, NULL,
		raydium_i2c_irq, IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			client->name, ts);
	if (error) {
		dev_err(&client->dev, "Failed to register interrupt\n");
		return error;
	}

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
			"Failed to add sysfs cleanup action: %d\n", error);
		return error;
	}

	return 0;
}

static void __maybe_unused raydium_enter_sleep(struct i2c_client *client)
{
	static const u8 sleep_cmd[] = { 0x5A, 0xff, 0x00, 0x0f };
	int error;

	error = raydium_i2c_send(client, CMD_ENTER_SLEEP, (u8 *)sleep_cmd,
		sizeof(sleep_cmd));
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
		return -ENOMEM;

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
	{ "RAYD0001", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, raydium_acpi_id);
#endif

#ifdef CONFIG_OF
static const struct of_device_id raydium_of_match[] = {
	{ .compatible = "raydium,rm32380",},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, raydium_of_match);
#endif

static struct i2c_driver raydium_i2c_driver = {
	.probe = raydium_i2c_probe,
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
MODULE_LICENSE("GPL v2");
