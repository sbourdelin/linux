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
#include <linux/gpio.h>
#include <linux/gpio/machine.h>
#include <linux/i2c.h>
#include <linux/acpi.h>
#include <linux/interrupt.h>
#include <linux/regmap.h>
#include <linux/sysfs.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/regulator/consumer.h>
#include <asm/unaligned.h>

#define HIDEEP_TS_NAME				"HiDeep Touchscreen"
#define HIDEEP_I2C_NAME				"hideep_ts"

#define HIDEEP_MT_MAX				10
#define HIDEEP_KEY_MAX				3
/* count(2) + touch data(100) + key data(6) */
#define HIDEEP_MAX_EVENT			108
#define HIDEEP_TOUCH_EVENT_INDEX		2
#define HIDEEP_KEY_EVENT_INDEX			102

/* Touch & key event */
#define HIDEEP_EVENT_ADDR			0x240

/* command list */
#define HIDEEP_RESET_CMD			0x9800

/* event bit */
#define HIDEEP_MT_RELEASED			BIT(4)
#define HIDEEP_KEY_PRESSED			BIT(7)
#define HIDEEP_KEY_FIRST_PRESSED		BIT(8)
#define HIDEEP_KEY_PRESSED_MASK \
	(HIDEEP_KEY_PRESSED | HIDEEP_KEY_FIRST_PRESSED)

/* For NVM */
#define HIDEEP_YRAM_BASE			0x40000000
#define HIDEEP_PERIPHERAL_BASE			0x50000000
#define HIDEEP_ESI_BASE \
	(HIDEEP_PERIPHERAL_BASE + 0x00000000)
#define HIDEEP_FLASH_BASE \
	(HIDEEP_PERIPHERAL_BASE + 0x01000000)
#define HIDEEP_SYSCON_BASE \
	(HIDEEP_PERIPHERAL_BASE + 0x02000000)

#define HIDEEP_SYSCON_MOD_CON			(HIDEEP_SYSCON_BASE + 0x0000)
#define HIDEEP_SYSCON_SPC_CON			(HIDEEP_SYSCON_BASE + 0x0004)
#define HIDEEP_SYSCON_CLK_CON			(HIDEEP_SYSCON_BASE + 0x0008)
#define HIDEEP_SYSCON_CLK_ENA			(HIDEEP_SYSCON_BASE + 0x000C)
#define HIDEEP_SYSCON_RST_CON			(HIDEEP_SYSCON_BASE + 0x0010)
#define HIDEEP_SYSCON_WDT_CON			(HIDEEP_SYSCON_BASE + 0x0014)
#define HIDEEP_SYSCON_WDT_CNT			(HIDEEP_SYSCON_BASE + 0x0018)
#define HIDEEP_SYSCON_PWR_CON			(HIDEEP_SYSCON_BASE + 0x0020)
#define HIDEEP_SYSCON_PGM_ID			(HIDEEP_SYSCON_BASE + 0x00F4)

#define HIDEEP_FLASH_CON			(HIDEEP_FLASH_BASE + 0x0000)
#define HIDEEP_FLASH_STA			(HIDEEP_FLASH_BASE + 0x0004)
#define HIDEEP_FLASH_CFG			(HIDEEP_FLASH_BASE + 0x0008)
#define HIDEEP_FLASH_TIM			(HIDEEP_FLASH_BASE + 0x000C)
#define HIDEEP_FLASH_CACHE_CFG			(HIDEEP_FLASH_BASE + 0x0010)
#define HIDEEP_FLASH_PIO_SIG			(HIDEEP_FLASH_BASE + 0x400000)

#define HIDEEP_ESI_TX_INVALID			(HIDEEP_ESI_BASE + 0x0008)

#define HIDEEP_PERASE				0x00040000
#define HIDEEP_WRONLY				0x00100000

#define HIDEEP_NVM_MASK_OFS			0x0000000C
#define HIDEEP_NVM_DEFAULT_PAGE			0
#define HIDEEP_NVM_SFR_WPAGE			1
#define HIDEEP_NVM_SFR_RPAGE			2

#define HIDEEP_PIO_SIG				0x00400000
#define HIDEEP_PROT_MODE			0x03400000

#define HIDEEP_NVM_PAGE_SIZE			128

#define HIDEEP_DWZ_INFO				0x000002C0

struct hideep_event {
	__le16 x;
	__le16 y;
	__le16 z;
	u8 w;
	u8 flag;
	u8 type;
	u8 index;
} __packed;

struct dwz_info {
	__le32	code_start;
	u8 code_crc[12];

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

	u8 factory_id;
	u8 panel_type;
	u8 model_name[6];
	__le16 product_code;
	__le16 extra_option;

	__le16 product_id;
	__le16 vendor_id;
} __packed;

struct hideep_ts {
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct regmap *reg;

	struct touchscreen_properties prop;

	struct gpio_desc *reset_gpio;

	struct regulator *vcc_vdd;
	struct regulator *vcc_vid;

	struct mutex dev_mutex;

	u32 tch_count;
	u32 key_count;
	u32 lpm_count;

	u8 touch_event[HIDEEP_MT_MAX * 10];
	u8 key_event[HIDEEP_KEY_MAX * 2];

	int key_num;
	int key_codes[HIDEEP_KEY_MAX];

	struct dwz_info dwz_info;

	int fw_size;
	int nvm_mask;
};

struct pgm_packet {
	union {
		u8 b[8];
		u32 w[2];
	} header;

	u32 payload[HIDEEP_NVM_PAGE_SIZE / sizeof(u32)];
};

static int hideep_pgm_w_mem(struct hideep_ts *ts, u32 addr,
	struct pgm_packet *packet, u32 len)
{
	int ret;
	int i;
	struct i2c_msg msg;

	if ((len % sizeof(u32)) != 0)
		return -EINVAL;

	put_unaligned_be32((0x80 | (len / sizeof(u32) - 1)),
		&packet->header.w[0]);
	put_unaligned_be32(addr, &packet->header.w[1]);

	for (i = 0; i < len / sizeof(u32); i++)
		put_unaligned_be32(packet->payload[i], &packet->payload[i]);

	msg.addr = ts->client->addr;
	msg.flags = 0;
	msg.len = len + 5;
	msg.buf = &packet->header.b[3];
	
	ret = i2c_transfer(ts->client->adapter, &msg, 1);

	return ret;
}

static int hideep_pgm_r_mem(struct hideep_ts *ts, u32 addr,
	struct pgm_packet *packet, u32 len)
{
	int ret;
	int i;
	u8 *buff;
	struct i2c_msg msg[2];

	if ((len % sizeof(u32)) != 0)
		return -EINVAL;

	buff = kmalloc(len, GFP_KERNEL);

	if (!buff)
		return -ENOMEM;

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
		return ret;

	for (i = 0; i < len / sizeof(u32); i++)
		packet->payload[i] = get_unaligned_be32(&buff[i * sizeof(u32)]);

	return ret;
}

static int hideep_pgm_r_reg(struct hideep_ts *ts, u32 addr,
	u32 *val)
{
	int ret;
	struct pgm_packet packet;

	put_unaligned_be32(0x00, &packet.header.w[0]);
	put_unaligned_be32(addr, &packet.header.w[1]);

	ret = hideep_pgm_r_mem(ts, addr, &packet, sizeof(u32));

	if (ret < 0)
		return ret;

	*val = packet.payload[0];

	return ret;
}

static int hideep_pgm_w_reg(struct hideep_ts *ts, u32 addr,
	u32 data)
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
	hideep_pgm_w_reg(ts, HIDEEP_SYSCON_WDT_CNT, CLK); \
	hideep_pgm_w_reg(ts, HIDEEP_SYSCON_WDT_CON, 0x03); \
	hideep_pgm_w_reg(ts, HIDEEP_SYSCON_WDT_CON, 0x01); \
}

#define SET_FLASH_PIO(CE) \
	hideep_pgm_w_reg(ts, HIDEEP_FLASH_CON, 0x01 | (CE << 1))
#define SET_PIO_SIG(X, Y) \
	hideep_pgm_w_reg(ts, HIDEEP_FLASH_PIO_SIG + X, Y)
#define SET_FLASH_HWCONTROL() \
	hideep_pgm_w_reg(ts, HIDEEP_FLASH_CON, 0x00)

#define NVM_W_SFR(x, y) \
{ \
	SET_FLASH_PIO(1); \
	SET_PIO_SIG(x, y); \
	SET_FLASH_PIO(0); \
}

static void hideep_pgm_set(struct hideep_ts *ts)
{
	hideep_pgm_w_reg(ts, HIDEEP_SYSCON_WDT_CON, 0x00);
	hideep_pgm_w_reg(ts, HIDEEP_SYSCON_SPC_CON, 0x00);
	hideep_pgm_w_reg(ts, HIDEEP_SYSCON_CLK_ENA, 0xFF);
	hideep_pgm_w_reg(ts, HIDEEP_SYSCON_CLK_CON, 0x01);
	hideep_pgm_w_reg(ts, HIDEEP_SYSCON_PWR_CON, 0x01);
	hideep_pgm_w_reg(ts, HIDEEP_FLASH_TIM, 0x03);
	hideep_pgm_w_reg(ts, HIDEEP_FLASH_CACHE_CFG, 0x00);
}

static int hideep_pgm_get_pattern(struct hideep_ts *ts)
{
	int ret;
	u32 status;
	u16 p1 = 0xAF39;
	u16 p2 = 0xDF9D;

	ret = regmap_bulk_write(ts->reg, p1, (void *)&p2, 1);

	if (ret < 0) {
		dev_err(&ts->client->dev, "%d, %08X", __LINE__, ret);
		return ret;
	}

	mdelay(1);

	/* flush invalid Tx load register */
	ret = hideep_pgm_w_reg(ts, HIDEEP_ESI_TX_INVALID, 0x01);

	if (ret < 0)
		return ret;

	ret = hideep_pgm_r_reg(ts, HIDEEP_SYSCON_PGM_ID, &status);

	if (ret < 0)
		return ret;

	return status;
}

static int hideep_enter_pgm(struct hideep_ts *ts)
{
	int retry_count = 10;
	int val;
	u32 pgm_pattern = 0xDF9DAF39;

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
	u32 unmask_code = 0;

	hideep_pgm_w_reg(ts, HIDEEP_FLASH_CFG,
		HIDEEP_NVM_SFR_RPAGE);

	hideep_pgm_r_reg(ts, 0x0000000C, &unmask_code);

	hideep_pgm_w_reg(ts, HIDEEP_FLASH_CFG,
		HIDEEP_NVM_DEFAULT_PAGE);

	/* make it unprotected code */
	unmask_code &= (~HIDEEP_PROT_MODE);

	/* compare unmask code */
	if (unmask_code != ts->nvm_mask)
		dev_dbg(&ts->client->dev, "read mask code different 0x%x",
			unmask_code);

	hideep_pgm_w_reg(ts, HIDEEP_FLASH_CFG,
		HIDEEP_NVM_SFR_WPAGE);
	SET_FLASH_PIO(0);

	NVM_W_SFR(HIDEEP_NVM_MASK_OFS, ts->nvm_mask);
	SET_FLASH_HWCONTROL();
	hideep_pgm_w_reg(ts, HIDEEP_FLASH_CFG,
		HIDEEP_NVM_DEFAULT_PAGE);
}

static int hideep_check_status(struct hideep_ts *ts)
{
	int ret, status;
	int time_out = 100;

	while (time_out--) {
		mdelay(1);
		ret = hideep_pgm_r_reg(ts, HIDEEP_FLASH_STA,
			&status);

		if (ret < 0)
			continue;

		if (status)
			return status;
	}

	return time_out;
}

static int hideep_program_page(struct hideep_ts *ts,
	u32 addr, struct pgm_packet *packet_w)
{
	int ret;


	ret = hideep_check_status(ts);

	if (ret < 0)
		return -EBUSY;

	addr = addr & ~(HIDEEP_NVM_PAGE_SIZE - 1);

	SET_FLASH_PIO(0);
	SET_FLASH_PIO(1);

	/* erase page */
	SET_PIO_SIG((HIDEEP_PERASE | addr), 0xFFFFFFFF);

	SET_FLASH_PIO(0);

	ret = hideep_check_status(ts);

	if (ret < 0)
		return -EBUSY;

	/* write page */
	SET_FLASH_PIO(1);

	SET_PIO_SIG((HIDEEP_WRONLY | addr),
		get_unaligned_be32(&packet_w->payload[0]));

	hideep_pgm_w_mem(ts, (HIDEEP_FLASH_PIO_SIG | HIDEEP_WRONLY),
		packet_w, HIDEEP_NVM_PAGE_SIZE);

	SET_PIO_SIG(124, get_unaligned_be32(&packet_w->payload[31]));

	SET_FLASH_PIO(0);

	mdelay(1);

	ret = hideep_check_status(ts);

	if (ret < 0)
		return -EBUSY;

	SET_FLASH_HWCONTROL();

	return 0;
}

static void hideep_program_nvm(struct hideep_ts *ts, const u8 *ucode,
	int len)
{
	struct pgm_packet packet_w;
	struct pgm_packet packet_r;
	int i;
	int ret;
	int addr = 0;
	int len_r = len;
	int len_w = HIDEEP_NVM_PAGE_SIZE;
	u32 pages = DIV_ROUND_UP(len, HIDEEP_NVM_PAGE_SIZE);


	hideep_nvm_unlock(ts);

	dev_dbg(&ts->client->dev, "pages : %d", pages);

	for (i = 0; i < pages; i++) {
		if (len_r < HIDEEP_NVM_PAGE_SIZE)
			len_w = len_r;

		/* compare */
		hideep_pgm_r_mem(ts, 0x00000000 + addr, &packet_r,
			HIDEEP_NVM_PAGE_SIZE);
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

		addr += HIDEEP_NVM_PAGE_SIZE;
		len_r -= HIDEEP_NVM_PAGE_SIZE;
		if (len_r < 0)
			break;
	}
}

static int hideep_verify_nvm(struct hideep_ts *ts, const u8 *ucode,
	int len)
{
	struct pgm_packet packet_r;
	int i, j;
	int ret;
	int addr = 0;
	int len_r = len;
	int len_v = HIDEEP_NVM_PAGE_SIZE;
	u32 pages = DIV_ROUND_UP(len, HIDEEP_NVM_PAGE_SIZE);

	for (i = 0; i < pages; i++) {
		if (len_r < HIDEEP_NVM_PAGE_SIZE)
			len_v = len_r;

		hideep_pgm_r_mem(ts, 0x00000000 + addr, &packet_r,
			HIDEEP_NVM_PAGE_SIZE);

		ret = memcmp(&ucode[addr], packet_r.payload, len_v);

		if (ret) {
			u8 *read = (u8 *)packet_r.payload;

			for (j = 0; j < HIDEEP_NVM_PAGE_SIZE; j++) {
				if (ucode[addr + j] != read[j])
					dev_err(&ts->client->dev,
						"verify : error([%d] %02x : %02x)",
						addr + j, ucode[addr + j],
						read[j]);
			}
			return ret;
		}

		addr += HIDEEP_NVM_PAGE_SIZE;
		len_r -= HIDEEP_NVM_PAGE_SIZE;
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
			"file size(%zu) is big more than fw memory size(%d)",
			fw_entry->size, ts->fw_size);
		release_firmware(fw_entry);
		return -EFBIG;
	}

	/* chip specific code for flash fuse */
	mutex_lock(&ts->dev_mutex);

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
	} else {
		dev_dbg(&ts->client->dev, "product code is wrong!!!");
		return -EINVAL;
	}

	dev_dbg(&ts->client->dev, "firmware release version : %04x",
		get_unaligned_le16(&ts->dwz_info.release_ver));

	mdelay(50);

	return 0;
}

static int hideep_pwr_on(struct hideep_ts *ts)
{
	int ret = 0;
	u8 cmd = 0x01;

	if (ts->vcc_vdd) {
		ret = regulator_enable(ts->vcc_vdd);
		if (ret)
			dev_err(&ts->client->dev,
				"Regulator vdd enable failed ret=%d", ret);
		usleep_range(999, 1000);
	}

	if (ts->vcc_vid) {
		ret = regulator_enable(ts->vcc_vid);
		if (ret)
			dev_err(&ts->client->dev,
				"Regulator vcc_vid enable failed ret=%d", ret);
		usleep_range(2999, 3000);
	}

	mdelay(30);

	if (ts->reset_gpio)
		gpiod_set_raw_value(ts->reset_gpio, 1);
	else
		regmap_write(ts->reg, HIDEEP_RESET_CMD, cmd);

	mdelay(50);

	return ret;
}

static void hideep_pwr_off(void *data)
{
	struct hideep_ts *ts = data;

	if (ts->reset_gpio)
		gpiod_set_value(ts->reset_gpio, 0);

	if (ts->vcc_vid)
		regulator_disable(ts->vcc_vid);

	if (ts->vcc_vdd)
		regulator_disable(ts->vcc_vdd);
}

#define __GET_MT_TOOL_TYPE(X) ((X == 0x01) ? MT_TOOL_FINGER : MT_TOOL_PEN)

static void push_mt(struct hideep_ts *ts)
{
	int id;
	int i;
	int btn_up = 0;
	int evt = 0;
	int offset = sizeof(struct hideep_event);
	struct hideep_event *event;

	/* load multi-touch event to input system */
	for (i = 0; i < ts->tch_count; i++) {
		event = (struct hideep_event *)&ts->touch_event[i * offset];
		id = event->index & 0x0F;
		btn_up = event->flag & HIDEEP_MT_RELEASED;

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
		code = ts->key_event[i * 2] & 0x0F;
		status = ts->key_event[i * 2] & 0xF0;

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

static int hideep_parse_event(struct hideep_ts *ts, u8 *data)
{
	int touch_count;

	ts->tch_count = data[0];
	ts->key_count = data[1] & 0x0f;
	ts->lpm_count = data[1] & 0xf0;

	/* get touch event count */
	dev_dbg(&ts->client->dev, "mt = %d, key = %d, lpm = %02x",
		ts->tch_count, ts->key_count, ts->lpm_count);

	/* get touch event information */
	if (ts->tch_count < HIDEEP_MT_MAX)
		memcpy(ts->touch_event, &data[HIDEEP_TOUCH_EVENT_INDEX],
			HIDEEP_MT_MAX * sizeof(struct hideep_event));
	else
		ts->tch_count = 0;

	if (ts->key_count < HIDEEP_KEY_MAX)
		memcpy(ts->key_event, &data[HIDEEP_KEY_EVENT_INDEX],
			HIDEEP_KEY_MAX * 2);
	else
		ts->key_count = 0;

	touch_count = ts->tch_count + ts->key_count;

	return touch_count;
}

static irqreturn_t hideep_irq_task(int irq, void *handle)
{
	u8 buff[HIDEEP_MAX_EVENT];
	int ret;

	struct hideep_ts *ts = handle;

	ret = regmap_bulk_read(ts->reg, HIDEEP_EVENT_ADDR,
		buff, HIDEEP_MAX_EVENT / 2);

	if (ret < 0)
		return IRQ_HANDLED;

	ret = hideep_parse_event(ts, buff);

	if (ret > 0)
		hideep_put_event(ts);

	return IRQ_HANDLED;
}

static void hideep_get_axis_info(struct hideep_ts *ts)
{
	int ret;
	u8 val[4];

	if (ts->prop.max_x == 0 || ts->prop.max_y == 0) {
		ret = regmap_bulk_read(ts->reg, 0x28, val, 2);

		if (ret < 0) {
			ts->prop.max_x = -1;
			ts->prop.max_y = -1;
		} else {
			ts->prop.max_x =
				get_unaligned_le16(&val[0]);
			ts->prop.max_y =
				get_unaligned_le16(&val[2]);
		}
	}

	dev_dbg(&ts->client->dev, "X : %d, Y : %d",
		ts->prop.max_x, ts->prop.max_y);
}

static int hideep_capability(struct hideep_ts *ts)
{
	int ret, i;

	hideep_get_axis_info(ts);

	if (ts->prop.max_x < 0 || ts->prop.max_y < 0)
		return -EINVAL;

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

	return count;
}

static ssize_t hideep_fw_version_show(struct device *dev,
	struct device_attribute *attr, char *buf)
{
	int len = 0;
	struct hideep_ts *ts = dev_get_drvdata(dev);

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
	int ret;

	ret = hideep_pwr_on(ts);
	if (ret < 0)
		dev_err(&ts->client->dev, "power on failed");
	else
		enable_irq(ts->client->irq);

	return ret;
}

static int __maybe_unused hideep_suspend(struct device *dev)
{
	struct hideep_ts *ts = dev_get_drvdata(dev);

	disable_irq(ts->client->irq);
	hideep_pwr_off(ts);

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

const struct regmap_config hideep_regmap_config = {
	.reg_bits = 16,
	.reg_format_endian = REGMAP_ENDIAN_LITTLE,
	.val_bits = 16,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
	.max_register = 0xffff,
};

static int hideep_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int ret;
	struct regmap *regmap;
	struct hideep_ts *ts;

	/* check i2c bus */
	if (!i2c_check_functionality(client->adapter,
		I2C_FUNC_I2C)) {
		dev_err(&client->dev, "check i2c device error");
		return -ENODEV;
	}

	regmap = devm_regmap_init_i2c(client, &hideep_regmap_config);

	if (IS_ERR(regmap)) {
		dev_err(&client->dev, "don't init regmap");
		return PTR_ERR(regmap);
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
	ts->reg = regmap;

	i2c_set_clientdata(client, ts);

	mutex_init(&ts->dev_mutex);

	/* power on */
	ret = hideep_pwr_on(ts);
	if (ret) {
		dev_err(&ts->client->dev, "power on failed");
		return ret;
	}

	ret = devm_add_action_or_reset(&ts->client->dev, hideep_pwr_off, ts);
	if (ret) {
		hideep_pwr_off(ts);
		return ret;
	}

	mdelay(30);

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
	if (client->irq <= 0) {
		dev_err(&client->dev, "can't be assigned irq");
		return -EINVAL;
	}

	ret = devm_request_threaded_irq(&client->dev, ts->client->irq,
		NULL, hideep_irq_task, IRQF_ONESHOT,
		ts->client->name, ts);

	if (ret < 0) {
		dev_err(&client->dev, "fail to get irq, ret = 0x%08x",
			ret);
		return ret;
	}

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
