/*
 * Copyright (C) 2012-2017 Hideep, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2
 * as published by the Free Software Foudation.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/sysfs.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <asm/unaligned.h>

#define HIDEEP_TS_NAME					"HiDeep Touchscreen"
#define HIDEEP_I2C_NAME					"hideep_ts"

#define HIDEEP_MT_MAX					10
#define HIDEEP_KEY_MAX					3
#define FRAME_HEADER_SIZE				8

/* Touch & key event */
#define HIDEEP_EVENT_COUNT_ADDR			0x240
#define HIDEEP_TOUCH_DATA_ADDR			0x242
#define HIDEEP_KEY_DATA_ADDR			0x2A6
#define HIDEEP_RAW_DATA_ADDR			0x1000

/* command list */
#define HIDEEP_RESET_CMD				0x9800
#define HIDEEP_INTCLR_CMD				0x9802
#define HIDEEP_OPMODE_CMD				0x9804
#define HIDEEP_SWTICH_CMD				0x9805
#define HIDEEP_SLEEP_CMD				0x980D

/* multi touch event bit */
#define HIDEEP_MT_ALWAYS_REPORT			0
#define HIDEEP_MT_TOUCHED				1
#define HIDEEP_MT_FIRST_CONTACT			2
#define HIDEEP_MT_DRAG_MOVE				3
#define HIDEEP_MT_RELEASED				4
#define HIDEEP_MT_PINCH					5
#define HIDEEP_MT_PRESSURE				6

/* key event bit */
#define HIDEEP_KEY_RELEASED				0x20
#define HIDEEP_KEY_PRESSED				0x40
#define HIDEEP_KEY_FIRST_PRESSED		0x80
#define HIDEEP_KEY_PRESSED_MASK			0xC0

/* For NVM */
#define YRAM_BASE				0x40000000
#define PERIPHERAL_BASE			0x50000000
#define ESI_BASE				(PERIPHERAL_BASE + 0x00000000)
#define FLASH_BASE				(PERIPHERAL_BASE + 0x01000000)
#define SYSCON_BASE				(PERIPHERAL_BASE + 0x02000000)

#define SYSCON_MOD_CON			(SYSCON_BASE + 0x0000)
#define SYSCON_SPC_CON			(SYSCON_BASE + 0x0004)
#define SYSCON_CLK_CON			(SYSCON_BASE + 0x0008)
#define SYSCON_CLK_ENA			(SYSCON_BASE + 0x000C)
#define SYSCON_RST_CON			(SYSCON_BASE + 0x0010)
#define SYSCON_WDT_CON			(SYSCON_BASE + 0x0014)
#define SYSCON_WDT_CNT			(SYSCON_BASE + 0x0018)
#define SYSCON_PWR_CON			(SYSCON_BASE + 0x0020)
#define SYSCON_PGM_ID			(SYSCON_BASE + 0x00F4)

#define FLASH_CON				(FLASH_BASE + 0x0000)
#define FLASH_STA				(FLASH_BASE + 0x0004)
#define FLASH_CFG				(FLASH_BASE + 0x0008)
#define FLASH_TIM				(FLASH_BASE + 0x000C)
#define FLASH_CACHE_CFG			(FLASH_BASE + 0x0010)
#define FLASH_PIO_SIG			(FLASH_BASE + 0x400000)

#define ESI_TX_INVALID			(ESI_BASE + 0x0008)

#define PERASE					0x00040000
#define WRONLY					0x00100000

#define NVM_MASK_OFS			0x0000000C
#define NVM_DEFAULT_PAGE		0
#define NVM_SFR_WPAGE			1
#define NVM_SFR_RPAGE			2

#define PIO_SIG					0x00400000
#define _PROT_MODE				0x03400000

#define NVM_PAGE_SIZE			128

#define HIDEEP_DWZ_INFO			0x000002C0

enum e_dev_state {
	state_init = 1,
	state_normal,
	state_sleep,
	state_updating,
};

struct hideep_event {
	__le16 x;
	__le16 y;
	__le16 z;
	unsigned char w;
	unsigned char flag;
	unsigned char type;
	unsigned char index;
} __packed;

struct dwz_info {
	__le32	code_start;
	unsigned char code_crc[12];

	__le32 c_code_start;
	__le16 c_code_len;
	__le16 gen_ver;

	__le32 vr_start;
	__le16 vr_len;
	__le16 rsv0;

	__le32 ft_start;
	__le16 ft_len;
	__le16 vr_version;

	__le16 boot_ver;
	__le16 core_ver;
	__le16 custom_ver;
	__le16 release_ver;

	unsigned char factory_id;
	unsigned char panel_type;
	unsigned char model_name[6];
	__le16 product_code;
	__le16 extra_option;

	__le16 product_id;
	__le16 vendor_id;
} __packed;

struct hideep_ts {
	struct i2c_client *client;
	struct input_dev *input_dev;

	struct touchscreen_properties prop;

	struct gpio_desc *reset_gpio;

	struct regulator *vcc_vdd;
	struct regulator *vcc_vid;

	struct mutex dev_mutex;
	struct mutex i2c_mutex;

	enum e_dev_state dev_state;

	unsigned int tch_count;
	unsigned int key_count;
	unsigned int lpm_count;

	unsigned char touch_event[HIDEEP_MT_MAX * 10];
	unsigned char key_event[HIDEEP_KEY_MAX * 2];

	int key_num;
	int key_codes[HIDEEP_KEY_MAX];

	struct dwz_info dwz_info;

	int fw_size;
	int nvm_mask;
} __packed;

struct pgm_packet {
	union {
		unsigned char b[8];
		__be32 w[2];
	} header;

	__be32 payload[NVM_PAGE_SIZE / sizeof(unsigned int)];
};

static void hideep_reset_ic(struct hideep_ts *ts);

static int hideep_pgm_w_mem(struct hideep_ts *ts, unsigned int addr,
	struct pgm_packet *packet, unsigned int len)
{
	int ret;
	int i;

	if ((len % sizeof(u32)) != 0)
		return -EINVAL;

	put_unaligned_be32((0x80 | (len / sizeof(u32) - 1)),
		&packet->header.w[0]);
	put_unaligned_be32(addr, &packet->header.w[1]);

	for (i = 0; i < len / sizeof(u32); i++)
		put_unaligned_be32(packet->payload[i], &packet->payload[i]);

	ret = i2c_master_send(ts->client, &packet->header.b[3],
		(len + 5));

	return ret;
}

static int hideep_pgm_r_mem(struct hideep_ts *ts, unsigned int addr,
	struct pgm_packet *packet, unsigned int len)
{
	int ret;
	int i;
	unsigned char buff[len];	// need to modify
	struct i2c_msg msg[2];

	if ((len % sizeof(u32)) != 0)
		return -EINVAL;

	put_unaligned_be32((0x00 | (len / sizeof(u32) - 1)),
		&packet->header.w[0]);
	put_unaligned_be32(addr, &packet->header.w[1]);

	msg[0].addr = ts->client->addr;
	msg[0].flags = 0;
	msg[0].len = 5;
	msg[0].buf = &packet->header.b[3];

	msg[1].addr = ts->client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = buff;

	ret = i2c_transfer(ts->client->adapter, msg, 2);

	if (ret < 0)
		goto err;

	for (i = 0; i < len / sizeof(u32); i++)
		packet->payload[i] = get_unaligned_be32(&buff[i * sizeof(u32)]);

err:
	return ret;
}

static int hideep_pgm_r_reg(struct hideep_ts *ts, unsigned int addr,
	unsigned int *val)
{
	int ret;
	struct pgm_packet packet;

	put_unaligned_be32(0x00, &packet.header.w[0]);
	put_unaligned_be32(addr, &packet.header.w[1]);

	ret = hideep_pgm_r_mem(ts, addr, &packet, sizeof(u32));

	if (ret < 0)
		goto err;

	*val = packet.payload[0];

err:
	return ret;
}

static int hideep_pgm_w_reg(struct hideep_ts *ts, unsigned int addr,
	unsigned int data)
{
	int ret;
	struct pgm_packet packet;

	put_unaligned_be32(0x80, &packet.header.w[0]);
	put_unaligned_be32(addr, &packet.header.w[1]);
	packet.payload[0] = data;

	ret = hideep_pgm_w_mem(ts, addr, &packet, sizeof(u32));

	return ret;
}

#define SW_RESET_IN_PGM(CLK) \
{ \
	hideep_pgm_w_reg(ts, SYSCON_WDT_CNT, CLK); \
	hideep_pgm_w_reg(ts, SYSCON_WDT_CON, 0x03); \
	hideep_pgm_w_reg(ts, SYSCON_WDT_CON, 0x01); \
}

#define SET_FLASH_PIO(CE) \
	hideep_pgm_w_reg(ts, FLASH_CON, 0x01 | (CE << 1))
#define SET_PIO_SIG(X, Y) \
	hideep_pgm_w_reg(ts, FLASH_PIO_SIG + X, Y)
#define SET_FLASH_HWCONTROL() \
	hideep_pgm_w_reg(ts, FLASH_CON, 0x00)

#define NVM_W_SFR(x, y) \
{ \
	SET_FLASH_PIO(1); \
	SET_PIO_SIG(x, y); \
	SET_FLASH_PIO(0); \
}

static void hideep_pgm_set(struct hideep_ts *ts)
{
	hideep_pgm_w_reg(ts, SYSCON_WDT_CON, 0x00);
	hideep_pgm_w_reg(ts, SYSCON_SPC_CON, 0x00);
	hideep_pgm_w_reg(ts, SYSCON_CLK_ENA, 0xFF);
	hideep_pgm_w_reg(ts, SYSCON_CLK_CON, 0x01);
	hideep_pgm_w_reg(ts, SYSCON_PWR_CON, 0x01);
	hideep_pgm_w_reg(ts, FLASH_TIM, 0x03);
	hideep_pgm_w_reg(ts, FLASH_CACHE_CFG, 0x00);
}

static int hideep_pgm_get_pattern(struct hideep_ts *ts)
{
	int ret;
	unsigned int status;
	const unsigned char pattern[] = { 0x39, 0xAF, 0x9D, 0xDF };

	ret = i2c_master_send(ts->client, pattern, sizeof(pattern));

	if (ret < 0) {
		dev_err(&ts->client->dev, "%d, %08X", __LINE__, ret);
		return ret;
	}

	mdelay(1);

	/* flush invalid Tx load register */
	ret = hideep_pgm_w_reg(ts, ESI_TX_INVALID, 0x01);

	if (ret < 0)
		return ret;

	ret = hideep_pgm_r_reg(ts, SYSCON_PGM_ID, &status);

	if (ret < 0)
		return ret;

	dev_info(&ts->client->dev, "%s, %08X", __func__, status);
	return status;
}

static int hideep_enter_pgm(struct hideep_ts *ts)
{
	int retry_count = 10;
	int val;
	unsigned int pgm_pattern = 0xDF9DAF39;

	while (retry_count--) {
		val = hideep_pgm_get_pattern(ts);

		if (pgm_pattern != get_unaligned_be32(&val)) {
			dev_err(&ts->client->dev, "enter_pgm : error(%08x):",
				get_unaligned_be32(&val));
		} else {
			dev_dbg(&ts->client->dev, "found magic code");
			break;
		}
	}


	if (retry_count < 0) {
		dev_err(&ts->client->dev, "couldn't enter pgm mode!!!");
		SW_RESET_IN_PGM(1000);
		return -EBADMSG;
	}

	hideep_pgm_set(ts);
	mdelay(1);

	return 0;
}

static void hideep_nvm_unlock(struct hideep_ts *ts)
{
	unsigned int unmask_code = 0;

	hideep_pgm_w_reg(ts, FLASH_CFG, NVM_SFR_RPAGE);

	hideep_pgm_r_reg(ts, 0x0000000C, &unmask_code);

	hideep_pgm_w_reg(ts, FLASH_CFG, NVM_DEFAULT_PAGE);

	/* make it unprotected code */
	unmask_code &= (~_PROT_MODE);

	/* compare unmask code */
	if (unmask_code != ts->nvm_mask)
		dev_dbg(&ts->client->dev, "read mask code different 0x%x",
			unmask_code);

	hideep_pgm_w_reg(ts, FLASH_CFG, NVM_SFR_WPAGE);
	SET_FLASH_PIO(0);

	NVM_W_SFR(NVM_MASK_OFS, ts->nvm_mask);
	SET_FLASH_HWCONTROL();
	hideep_pgm_w_reg(ts, FLASH_CFG, NVM_DEFAULT_PAGE);
}

static int hideep_check_status(struct hideep_ts *ts)
{
	int ret, status;
	int time_out = 100;

	while (time_out--) {
		mdelay(1);
		ret = hideep_pgm_r_reg(ts, FLASH_STA, &status);

		if (ret < 0)
			continue;

		if (status)
			return status;
	}

	return time_out;
}

static int hideep_program_page(struct hideep_ts *ts,
	unsigned int addr, struct pgm_packet *packet_w)
{
	int ret;


	ret = hideep_check_status(ts);

	if (ret < 0)
		return -EBUSY;

	addr = addr & ~(NVM_PAGE_SIZE - 1);

	SET_FLASH_PIO(0);
	SET_FLASH_PIO(1);

	/* erase page */
	SET_PIO_SIG((PERASE | addr), 0xFFFFFFFF);

	SET_FLASH_PIO(0);

	ret = hideep_check_status(ts);

	if (ret < 0)
		return -EBUSY;

	/* write page */
	SET_FLASH_PIO(1);

	SET_PIO_SIG((WRONLY | addr), get_unaligned_be32(&packet_w->payload[0]));

	hideep_pgm_w_mem(ts, (FLASH_PIO_SIG | WRONLY),
		packet_w, NVM_PAGE_SIZE);

	SET_PIO_SIG(124, get_unaligned_be32(&packet_w->payload[31]));

	SET_FLASH_PIO(0);

	mdelay(1);

	ret = hideep_check_status(ts);

	if (ret < 0)
		return -EBUSY;

	SET_FLASH_HWCONTROL();

	return 0;
}

static void hideep_program_nvm(struct hideep_ts *ts, const unsigned char *ucode,
	int len)
{
	struct pgm_packet packet_w;
	struct pgm_packet packet_r;
	int i;
	int ret;
	int addr = 0;
	int len_r = len;
	int len_w = NVM_PAGE_SIZE;
	unsigned int pages = DIV_ROUND_UP(len, NVM_PAGE_SIZE);


	hideep_nvm_unlock(ts);

	dev_dbg(&ts->client->dev, "pages : %d", pages);

	for (i = 0; i < pages; i++) {
		if (len_r < NVM_PAGE_SIZE)
			len_w = len_r;

		/* compare */
		hideep_pgm_r_mem(ts, 0x00000000 + addr, &packet_r,
			NVM_PAGE_SIZE);
		ret = memcmp(&ucode[addr], packet_r.payload, len_w);

		if (ret) {
			/* write page */
			memcpy(packet_w.payload, &ucode[addr], len_w);
			ret = hideep_program_page(ts, addr, &packet_w);
			if (ret)
				dev_err(&ts->client->dev,
					"%s : error(%08x):",
					__func__, addr);
			mdelay(1);
		}

		addr += NVM_PAGE_SIZE;
		len_r -= NVM_PAGE_SIZE;
		if (len_r < 0)
			break;
	}
}

static int hideep_verify_nvm(struct hideep_ts *ts, const unsigned char *ucode,
	int len)
{
	struct pgm_packet packet_r;
	int i, j;
	int ret;
	int addr = 0;
	int len_r = len;
	int len_v = NVM_PAGE_SIZE;
	unsigned int pages = DIV_ROUND_UP(len, NVM_PAGE_SIZE);

	for (i = 0; i < pages; i++) {
		if (len_r < NVM_PAGE_SIZE)
			len_v = len_r;

		hideep_pgm_r_mem(ts, 0x00000000 + addr, &packet_r,
			NVM_PAGE_SIZE);

		ret = memcmp(&ucode[addr], packet_r.payload, len_v);

		if (ret) {
			u8 *read = (u8 *)packet_r.payload;

			for (j = 0; j < NVM_PAGE_SIZE; j++) {
				if (ucode[addr + j] != read[j])
					dev_err(&ts->client->dev,
						"verify : error([%d] %02x : %02x)",
						addr + j, ucode[addr + j],
						read[j]);
			}
			return ret;
		}

		addr += NVM_PAGE_SIZE;
		len_r -= NVM_PAGE_SIZE;
		if (len_r < 0)
			break;
	}

	return 0;
}

static int hideep_update_firmware(struct hideep_ts *ts, const char *fn)
{
	int ret;
	int retry, retry_cnt = 3;
	const struct firmware *fw_entry;

	dev_dbg(&ts->client->dev, "enter");
	ret = request_firmware(&fw_entry, fn, &ts->client->dev);

	if (ret != 0) {
		dev_err(&ts->client->dev, "request_firmware : fail(%d)", ret);
		return ret;
	}

	if (fw_entry->size > ts->fw_size) {
		dev_err(&ts->client->dev,
			"file size(%ld) is big more than fw memory size(%d)",
			fw_entry->size, ts->fw_size);
		release_firmware(fw_entry);
		return -EFBIG;
	}

	/* chip specific code for flash fuse */
	mutex_lock(&ts->dev_mutex);

	ts->dev_state = state_updating;

	/* enter program mode */
	ret = hideep_enter_pgm(ts);

	if (ret)
		return ret;

	/* comparing & programming each page, if the memory of specified
	 * page is exactly same, no need to update.
	 */
	for (retry = 0; retry < retry_cnt; retry++) {
		hideep_program_nvm(ts, fw_entry->data, fw_entry->size);

		ret = hideep_verify_nvm(ts, fw_entry->data, fw_entry->size);
		if (!ret)
			break;
	}

	if (retry < retry_cnt)
		dev_dbg(&ts->client->dev, "update success!!!");
	else
		dev_err(&ts->client->dev, "update failed!!!");

	SW_RESET_IN_PGM(1000);

	ts->dev_state = state_normal;

	mutex_unlock(&ts->dev_mutex);

	release_firmware(fw_entry);

	return ret;
}

static int hideep_load_dwz(struct hideep_ts *ts)
{
	int ret = 0;
	struct pgm_packet packet_r;

	ret = hideep_enter_pgm(ts);

	if (ret)
		return ret;

	mdelay(50);

	hideep_pgm_r_mem(ts, HIDEEP_DWZ_INFO, &packet_r,
		sizeof(struct dwz_info));

	memcpy(&ts->dwz_info, packet_r.payload,
		sizeof(struct dwz_info));

	SW_RESET_IN_PGM(10);

	if (get_unaligned_le16(&ts->dwz_info.product_code) & 0x60) {
		/* Lime fw size */
		dev_dbg(&ts->client->dev, "used lime IC");
		ts->fw_size = 1024 * 64;
		ts->nvm_mask = 0x0030027B;
	} else if (get_unaligned_le16(&ts->dwz_info.product_code) & 0x40) {
		/* Crimson IC */
		dev_dbg(&ts->client->dev, "used crimson IC");
		ts->fw_size = 1024 * 48;
		ts->nvm_mask = 0x00310000;
	}

	dev_dbg(&ts->client->dev, "firmware release version : %04x",
		get_unaligned_le16(&ts->dwz_info.release_ver));

	mdelay(50);

	return 0;
}

static int hideep_i2c_read(struct hideep_ts *ts, unsigned short addr,
	unsigned short len, unsigned char *buf)
{
	int ret;
	struct i2c_msg msg[2];

	mutex_lock(&ts->i2c_mutex);

	dev_dbg(&ts->client->dev, "addr = 0x%02x, len = %d", addr, len);

	msg[0].addr = ts->client->addr;
	msg[0].flags = 0;
	msg[0].len = 2;
	msg[0].buf = (u8 *)&addr;

	msg[1].addr = ts->client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len = len;
	msg[1].buf = buf;

	ret = i2c_transfer(ts->client->adapter, msg, 2);

	mutex_unlock(&ts->i2c_mutex);
	return ret;
}

static int hideep_i2c_write(struct hideep_ts *ts, unsigned short addr,
	unsigned short len, unsigned char *buf)
{
	int ret;
	unsigned char *wbuf;
	struct i2c_msg msg;

	dev_dbg(&ts->client->dev, "addr = 0x%02x, len = %d", addr, len);

	wbuf = kmalloc(len + 2, GFP_KERNEL);

	mutex_lock(&ts->i2c_mutex);

	put_unaligned_le16(addr, &wbuf[0]);
	memcpy(&wbuf[2], buf, len);

	msg.addr = ts->client->addr;
	msg.flags = 0;
	msg.len = len + 2;
	msg.buf = wbuf;

	ret = i2c_transfer(ts->client->adapter, &msg, 1);

	mutex_unlock(&ts->i2c_mutex);

	kfree(wbuf);

	return  ret;
}

static void hideep_reset_ic(struct hideep_ts *ts)
{
	unsigned char cmd = 0x01;

	if (!ts->reset_gpio) {
		dev_dbg(&ts->client->dev, "hideep:enable the reset_gpio");
		gpiod_set_value(ts->reset_gpio, GPIOD_OUT_LOW);
		mdelay(20);
		gpiod_set_value(ts->reset_gpio, GPIOD_OUT_HIGH);
	} else {
		hideep_i2c_write(ts, HIDEEP_RESET_CMD, 1, &cmd);
	}
	mdelay(50);
}

static int hideep_pwr_on(struct hideep_ts *ts)
{
	int ret = 0;

	if (!ts->vcc_vdd) {
		dev_dbg(&ts->client->dev, "hideep:vcc_vdd is enable");
		ret = regulator_enable(ts->vcc_vdd);
		if (ret)
			dev_err(&ts->client->dev,
				"Regulator vdd enable failed ret=%d", ret);
		usleep_range(999, 1000);
	}


	if (!ts->vcc_vid) {
		dev_dbg(&ts->client->dev, "hideep:vcc_vid is enable");
		ret = regulator_enable(ts->vcc_vid);
		if (ret)
			dev_err(&ts->client->dev,
				"Regulator vcc_vid enable failed ret=%d", ret);
		usleep_range(2999, 3000);
	}

	return ret;
}

static void hideep_pwr_off(void *data)
{
	struct hideep_ts *ts = data;

	if (!ts->reset_gpio)
		gpiod_set_value(ts->reset_gpio, GPIOD_OUT_LOW);

	if (!ts->vcc_vid)
		regulator_disable(ts->vcc_vid);

	if (!ts->vcc_vdd)
		regulator_disable(ts->vcc_vdd);
}

#define __GET_MT_TOOL_TYPE(X) ((X == 0x01) ? MT_TOOL_FINGER : MT_TOOL_PEN)

static void push_mt(struct hideep_ts *ts)
{
	int id;
	int i;
	bool btn_up = 0;
	int evt = 0;
	int offset = sizeof(struct hideep_event);
	struct hideep_event *event;

	/* load multi-touch event to input system */
	for (i = 0; i < ts->tch_count; i++) {
		event = (struct hideep_event *)&ts->touch_event[i * offset];
		id = (event->index >> 0) & 0x0F;
		btn_up = (event->flag >> HIDEEP_MT_RELEASED) & 0x01;

		dev_dbg(&ts->client->dev,
			"type = %d, id = %d, i = %d, x = %d, y = %d, z = %d",
			event->type, event->index, i,
			get_unaligned_le16(&event->x),
			get_unaligned_le16(&event->y),
			get_unaligned_le16(&event->z));

		input_mt_slot(ts->input_dev, id);
		input_mt_report_slot_state(ts->input_dev,
			__GET_MT_TOOL_TYPE(event->type),
			(btn_up == 0));

		if (btn_up == 0) {
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
				get_unaligned_le16(&event->x));
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
				get_unaligned_le16(&event->y));
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
				get_unaligned_le16(&event->z));
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
				event->w);
			evt++;
		}
	}

	input_mt_sync_frame(ts->input_dev);
}

static void push_ky(struct hideep_ts *ts)
{
	int i;
	int status;
	int code;

	for (i = 0; i < ts->key_count; i++) {
		code = ts->key_event[i + i * 2] & 0x0F;
		status = ts->key_event[i + i * 2] & 0xF0;

		input_report_key(ts->input_dev, ts->key_codes[code],
			status & HIDEEP_KEY_PRESSED_MASK);
	}
}

static void hideep_put_event(struct hideep_ts *ts)
{
	/* mangling touch information */
	if (ts->tch_count > 0)
		push_mt(ts);

	if (ts->key_count > 0)
		push_ky(ts);

	input_sync(ts->input_dev);
}

static int hideep_get_event(struct hideep_ts *ts)
{
	int ret;
	int touch_count;
	int event_size;

	/* get touch event count */
	dev_dbg(&ts->client->dev, "mt = %d, key = %d, lpm = %02x",
		ts->tch_count, ts->key_count, ts->lpm_count);

	/* get touch event information */
	if (ts->tch_count > HIDEEP_MT_MAX)
		ts->tch_count = 0;

	if (ts->key_count > HIDEEP_KEY_MAX)
		ts->key_count = 0;

	touch_count = ts->tch_count + ts->key_count;

	if (ts->tch_count > 0) {
		event_size = ts->tch_count *
			sizeof(struct hideep_event);
		ret = hideep_i2c_read(ts, HIDEEP_TOUCH_DATA_ADDR,
			event_size, ts->touch_event);

		if (ret < 0) {
			dev_err(&ts->client->dev, "read I2C error.");
			return ret;
		}
	}

	if (ts->key_count > 0) {
		event_size = ts->key_count * 2;
		ret = hideep_i2c_read(ts, HIDEEP_KEY_DATA_ADDR,
			event_size, ts->key_event);
		if (ret < 0) {
			dev_err(&ts->client->dev, "read I2C error.");
			return ret;
		}
	}

	return touch_count;
}

static irqreturn_t hideep_irq_task(int irq, void *handle)
{
	unsigned char buff[2];
	int ret;

	struct hideep_ts *ts = handle;

	dev_dbg(&ts->client->dev, "state = 0x%x", ts->dev_state);

	if (ts->dev_state == state_normal) {
		ret = hideep_i2c_read(ts, HIDEEP_EVENT_COUNT_ADDR,
			2, buff);

		if (ret < 0) {
			disable_irq(ts->client->irq);
			return IRQ_HANDLED;
		}

		ts->tch_count = buff[0];
		ts->key_count = buff[1] & 0x0f;
		ts->lpm_count = buff[1] & 0xf0;

		ret = hideep_get_event(ts);

		if (ret >= 0)
			hideep_put_event(ts);
	}

	return IRQ_HANDLED;
}

static int hideep_capability(struct hideep_ts *ts)
{
	int ret, i;

	ts->input_dev->name = HIDEEP_TS_NAME;
	ts->input_dev->id.bustype = BUS_I2C;

	if (ts->key_num) {
		ts->input_dev->keycode = ts->key_codes;
		ts->input_dev->keycodesize = sizeof(ts->key_codes[0]);
		ts->input_dev->keycodemax = ts->key_num;
		for (i = 0; i < ts->key_num; i++)
			input_set_capability(ts->input_dev, EV_KEY,
				ts->key_codes[i]);
	}

	input_set_abs_params(ts->input_dev,
		ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX, 0, 0);
	input_set_abs_params(ts->input_dev,
		ABS_MT_POSITION_X, 0, ts->prop.max_x, 0, 0);
	input_set_abs_params(ts->input_dev,
		ABS_MT_POSITION_Y, 0, ts->prop.max_y, 0, 0);
	input_set_abs_params(ts->input_dev,
		ABS_MT_PRESSURE, 0, 65535, 0, 0);
	input_set_abs_params(ts->input_dev,
		ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);

	ret = input_mt_init_slots(ts->input_dev,
		HIDEEP_MT_MAX, INPUT_MT_DIRECT);

	return ret;
}

static void hideep_get_info(struct hideep_ts *ts)
{
	unsigned char val[4];

	if (ts->prop.max_x == 0 || ts->prop.max_y == 0) {
		hideep_i2c_read(ts, 0x28, 4, val);

		ts->prop.max_x = get_unaligned_le16(&val[2]);
		ts->prop.max_y = get_unaligned_le16(&val[0]);

		dev_info(&ts->client->dev, "X : %d, Y : %d",
			ts->prop.max_x, ts->prop.max_y);
	}
}

static ssize_t hideep_update_fw(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct hideep_ts *ts = dev_get_drvdata(dev);
	int mode, ret;
	char *fw_name;

	ret = kstrtoint(buf, 8, &mode);
	if (ret)
		return ret;

	disable_irq(ts->client->irq);

	ts->dev_state = state_updating;
	fw_name = kasprintf(GFP_KERNEL, "hideep_ts_%04x.bin",
		get_unaligned_le16(&ts->dwz_info.product_id));
	ret = hideep_update_firmware(ts, fw_name);

	if (ret != 0)
		dev_err(dev, "The firmware update failed(%d)", ret);

	kfree(fw_name);

	ret = hideep_load_dwz(ts);

	if (ret < 0)
		dev_err(&ts->client->dev, "fail to load dwz, ret = 0x%x", ret);

	enable_irq(ts->client->irq);

	ts->dev_state = state_normal;

	return count;
}

static ssize_t hideep_fw_version_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int len = 0;
	struct hideep_ts *ts = dev_get_drvdata(dev);

	dev_info(dev, "release version : %04x",
		get_unaligned_le16(&ts->dwz_info.release_ver));

	mutex_lock(&ts->dev_mutex);
	len = scnprintf(buf, PAGE_SIZE,
		"%04x\n", get_unaligned_le16(&ts->dwz_info.release_ver));
	mutex_unlock(&ts->dev_mutex);

	return len;
}

static ssize_t hideep_product_id_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int len = 0;
	struct hideep_ts *ts = dev_get_drvdata(dev);

	dev_info(dev, "product id : %04x",
		get_unaligned_le16(&ts->dwz_info.product_id));

	mutex_lock(&ts->dev_mutex);
	len = scnprintf(buf, PAGE_SIZE,
		"%04x\n", get_unaligned_le16(&ts->dwz_info.product_id));
	mutex_unlock(&ts->dev_mutex);

	return len;
}

static DEVICE_ATTR(version, 0664, hideep_fw_version_show, NULL);
static DEVICE_ATTR(product_id, 0664, hideep_product_id_show, NULL);
static DEVICE_ATTR(update_fw, 0664, NULL, hideep_update_fw);

static struct attribute *hideep_ts_sysfs_entries[] = {
	&dev_attr_version.attr,
	&dev_attr_product_id.attr,
	&dev_attr_update_fw.attr,
	NULL,
};

static struct attribute_group hideep_ts_attr_group = {
	.attrs = hideep_ts_sysfs_entries,
};

static int __maybe_unused hideep_resume(struct device *dev)
{
	struct hideep_ts *ts = dev_get_drvdata(dev);
	unsigned char sleep_cmd = 0x00;

	mutex_lock(&ts->dev_mutex);

	if (ts->dev_state != state_normal)
		ts->dev_state = state_normal;

	hideep_i2c_write(ts, HIDEEP_SLEEP_CMD, 1, &sleep_cmd);
	enable_irq(ts->client->irq);

	mdelay(10);
	hideep_reset_ic(ts);

	mutex_unlock(&ts->dev_mutex);
	return 0;
}

static int __maybe_unused hideep_suspend(struct device *dev)
{
	struct hideep_ts *ts = dev_get_drvdata(dev);
	unsigned char sleep_cmd = 0x01;

	mutex_lock(&ts->dev_mutex);

	if (ts->dev_state != state_sleep)
		ts->dev_state = state_sleep;

	/* default deep sleep */
	hideep_i2c_write(ts, HIDEEP_SLEEP_CMD, 1, &sleep_cmd);
	disable_irq(ts->client->irq);

	mutex_unlock(&ts->dev_mutex);
	return 0;
}

static int  hideep_parse_dts(struct device *dev, struct hideep_ts *ts)
{
	int ret;

	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset",
							GPIOD_OUT_HIGH);
	if (IS_ERR(ts->reset_gpio))
		return PTR_ERR(ts->reset_gpio);

	ts->vcc_vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ts->vcc_vdd))
		return PTR_ERR(ts->vcc_vdd);

	ts->vcc_vid = devm_regulator_get(dev, "vid");
	if (IS_ERR(ts->vcc_vid))
		return PTR_ERR(ts->vcc_vid);

	ts->key_num = device_property_read_u32_array(dev, "linux,keycodes",
						NULL, 0);

	if (ts->key_num > HIDEEP_KEY_MAX) {
		dev_err(dev, "too many support key defined(%d)!!!",
			ts->key_num);
		return -EINVAL;
	}

	ret = device_property_read_u32_array(dev, "linux,keycodes",
				ts->key_codes, ts->key_num);
	if (ret) {
		dev_dbg(dev, "don't support touch key");
		ts->key_num = 0;
	}

	return 0;
}

static int hideep_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret = 0;
	struct hideep_ts *ts;

	/* check i2c bus */
	if (!i2c_check_functionality(client->adapter,
		I2C_FUNC_I2C)) {
		dev_err(&client->dev, "check i2c device error");
		return -ENODEV;
	}

	/* init hideep_ts */
	ts = devm_kzalloc(&client->dev,
		sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ret = hideep_parse_dts(&client->dev, ts);

	if (ret)
		return ret;

	ts->client = client;

	i2c_set_clientdata(client, ts);

	mutex_init(&ts->i2c_mutex);
	mutex_init(&ts->dev_mutex);

	/* power on */
	ret = hideep_pwr_on(ts);
	if (ret) {
		dev_err(&ts->client->dev, "power on failed");
		return ret;
	}

	ret = devm_add_action(&ts->client->dev, hideep_pwr_off, ts);
	if (ret) {
		hideep_pwr_off(ts);
		return ret;
	}

	ts->dev_state = state_init;
	mdelay(30);

	/* ic reset */
	hideep_reset_ic(ts);

	/* read info */
	ret = hideep_load_dwz(ts);
	if (ret < 0) {
		dev_err(&client->dev, "fail to load dwz, ret = 0x%x", ret);
		return ret;
	}

	/* init input device */
	ts->input_dev = devm_input_allocate_device(&client->dev);
	if (!ts->input_dev) {
		dev_err(&client->dev, "can't allocate memory for input_dev");
		return -ENOMEM;
	}

	touchscreen_parse_properties(ts->input_dev, true, &ts->prop);
	hideep_get_info(ts);

	ret = hideep_capability(ts);
	if (ret) {
		dev_err(&client->dev, "can't init input properties");
		return ret;
	}

	ret = input_register_device(ts->input_dev);
	if (ret) {
		dev_err(&client->dev, "can't register input_dev");
		return ret;
	}

	input_set_drvdata(ts->input_dev, ts);

	dev_info(&ts->client->dev, "ts irq: %d", ts->client->irq);
	if (IS_ERR(&ts->client->irq)) {
		dev_err(&client->dev, "can't be assigned irq");
		return -ENOMEM;
	}

	ret = devm_request_threaded_irq(&client->dev, ts->client->irq,
		NULL, hideep_irq_task, IRQF_ONESHOT,
		ts->client->name, ts);

	disable_irq(ts->client->irq);

	if (ret < 0) {
		dev_err(&client->dev, "fail to get irq, ret = 0x%08x",
			ret);
		return ret;
	}

	ts->dev_state = state_normal;
	enable_irq(ts->client->irq);

	ret = devm_device_add_group(&client->dev, &hideep_ts_attr_group);

	if (ret) {
		dev_err(&client->dev, "fail init sys, ret = 0x%x", ret);
		return ret;
	}

	return 0;
}

static SIMPLE_DEV_PM_OPS(hideep_pm_ops, hideep_suspend, hideep_resume);

static const struct i2c_device_id hideep_dev_idtable[] = {
	{ HIDEEP_I2C_NAME, 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, hideep_dev_idtable);

#ifdef CONFIG_ACPI
static const struct acpi_device_id hideep_acpi_id[] = {
	{ "HIDP0001", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, hideep_acpi_id);
#endif

#ifdef CONFIG_OF
static const struct of_device_id hideep_match_table[] = {
	{ .compatible = "hideep,hideep-ts" },
	{}
};
MODULE_DEVICE_TABLE(of, hideep_match_table);
#endif

static struct i2c_driver hideep_driver = {
	.probe = hideep_probe,
	.id_table = hideep_dev_idtable,
	.driver = {
		.name = HIDEEP_I2C_NAME,
		.of_match_table = of_match_ptr(hideep_match_table),
		.acpi_match_table = ACPI_PTR(hideep_acpi_id),
		.pm = &hideep_pm_ops,
	},
};

module_i2c_driver(hideep_driver);

MODULE_DESCRIPTION("Driver for HiDeep Touchscreen Controller");
MODULE_AUTHOR("anthony.kim@hideep.com");
MODULE_LICENSE("GPL v2");
