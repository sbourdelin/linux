// SPDX-License-Identifier: GPL-2.0
/*
 * UCSI driver for Cypress CCGx Type-C controller
 *
 * Copyright (C) 2017-2018 NVIDIA Corporation. All rights reserved.
 * Author: Ajay Gupta <ajayg@nvidia.com>
 *
 * Some code borrowed from drivers/usb/typec/ucsi/ucsi_acpi.c
 */
#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/platform_device.h>

#include <asm/unaligned.h>
#include "ucsi.h"

static int secondary_fw_min_ver = 41;
module_param(secondary_fw_min_ver, int, 0660);
#define CCGX_RAB_DEVICE_MODE			0x0000
#define CCGX_RAB_INTR_REG			0x0006
#define  DEV_INT				BIT(0)
#define  PORT0_INT				BIT(1)
#define  PORT1_INT				BIT(2)
#define  UCSI_READ_INT				BIT(7)
#define CCGX_RAB_JUMP_TO_BOOT			0x0007
#define  TO_BOOT				'J'
#define  TO_ALT_FW				'A'
#define CCGX_RAB_RESET_REQ			0x0008
#define  RESET_SIG				'R'
#define  CMD_RESET_I2C				0x0
#define  CMD_RESET_DEV				0x1
#define CCGX_RAB_ENTER_FLASHING			0x000A
#define  FLASH_ENTER_SIG			'P'
#define CCGX_RAB_VALIDATE_FW			0x000B
#define CCGX_RAB_FLASH_ROW_RW			0x000C
#define  FLASH_SIG				'F'
#define  FLASH_RD_CMD				0x0
#define  FLASH_WR_CMD				0x1
#define  FLASH_FWCT1_WR_CMD			0x2
#define  FLASH_FWCT2_WR_CMD			0x3
#define  FLASH_FWCT_SIG_WR_CMD			0x4
#define CCGX_RAB_READ_ALL_VER			0x0010
#define CCGX_RAB_READ_FW2_VER			0x0020
#define CCGX_RAB_UCSI_CONTROL			0x0039
#define CCGX_RAB_UCSI_CONTROL_START		BIT(0)
#define CCGX_RAB_UCSI_CONTROL_STOP		BIT(1)
#define CCGX_RAB_UCSI_DATA_BLOCK(offset)	(0xf000 | ((offset) & 0xff))
#define REG_FLASH_RW_MEM        0x0200
#define DEV_REG_IDX				CCGX_RAB_DEVICE_MODE
#define CCGX_RAB_PDPORT_ENABLE			0x002C
#define  PDPORT_1		BIT(0)
#define  PDPORT_2		BIT(1)
#define CCGX_RAB_RESPONSE			0x007E
#define  ASYNC_EVENT				BIT(7)

/* CCGx events & async msg codes */
#define RESET_COMPLETE		0x80
#define EVENT_INDEX		RESET_COMPLETE
#define PORT_CONNECT_DET	0x84
#define PORT_DISCONNECT_DET	0x85
#define ROLE_SWAP_COMPELETE	0x87

/* ccg firmware */
#define CYACD_LINE_SIZE         527
#define CCG4_ROW_SIZE           256
#define FW1_METADATA_ROW        0x1FF
#define FW2_METADATA_ROW        0x1FE
#define FW_CFG_TABLE_SIG_SIZE	256

enum enum_fw_mode {
	BOOT,   /* bootloader */
	FW1,    /* FW partition-1 */
	FW2,    /* FW partition-2 */
	FW_INVALID,
};

enum enum_flash_mode {
	SECONDARY_BL,	/* update secondary using bootloader */
	SECONDARY,	/* update secondary using primary */
	PRIMARY,	/* update primary */
	FLASH_NOT_NEEDED,	/* update not required */
	FLASH_INVALID,
};

static const char * const ccg_fw_names[] = {
	/* 0x00 */ "ccg_boot.cyacd",
	/* 0x01 */ "ccg_2.cyacd",
	/* 0x02 */ "ccg_1.cyacd",
};

struct ccg_dev_info {
	u8 fw_mode:2;
	u8 two_pd_ports:2;
	u8 row_size_256:2;
	u8:1; /* reserved */
	u8 hpi_v2_mode:1;
	u8 bl_mode:1;
	u8 cfgtbl_invalid:1;
	u8 fw1_invalid:1;
	u8 fw2_invalid:1;
	u8:4; /* reserved */
	u16 silicon_id;
	u16 bl_last_row;
} __packed;

struct version_format {
	u16 build;
	u8 patch;
	u8 min:4;
	u8 maj:4;
};

struct version_info {
	struct version_format base;
	struct version_format app;
};

struct fw_config_table {
	u32 identity;
	u16 table_size;
	u8 fwct_version;
	u8 is_key_change;
	u8 guid[16];
	struct version_format base;
	struct version_format app;
	u8 primary_fw_digest[32];
	u32 key_exp_length;
	u8 key_modulus[256];
	u8 key_exp[4];
};

/* CCGx response codes */
enum ccg_resp_code {
	CMD_NO_RESP             = 0x00,
	CMD_SUCCESS             = 0x02,
	FLASH_DATA_AVAILABLE    = 0x03,
	CMD_INVALID             = 0x05,
	FLASH_UPDATE_FAIL       = 0x07,
	INVALID_FW              = 0x08,
	INVALID_ARG             = 0x09,
	CMD_NOT_SUPPORT         = 0x0A,
	TRANSACTION_FAIL        = 0x0C,
	PD_CMD_FAIL             = 0x0D,
	UNDEF_ERROR             = 0x0F,
	INVALID_RESP		= 0x10,
};

static const char * const ccg_resp_strs[] = {
	/* 0x00 */ "No Response.",
	/* 0x01 */ "0x01",
	/* 0x02 */ "HPI Command Success.",
	/* 0x03 */ "Flash Data Available in data memory.",
	/* 0x04 */ "0x04",
	/* 0x05 */ "Invalid Command.",
	/* 0x06 */ "0x06",
	/* 0x07 */ "Flash write operation failed.",
	/* 0x08 */ "Firmware validity check failed.",
	/* 0x09 */ "Command failed due to invalid arguments.",
	/* 0x0A */ "Command not supported in the current mode.",
	/* 0x0B */ "0x0B",
	/* 0x0C */ "Transaction Failed. GOOD_CRC was not received.",
	/* 0x0D */ "PD Command Failed.",
	/* 0x0E */ "0x0E",
	/* 0x0F */ "Undefined Error",
};

static const char * const ccg_evt_strs[] = {
	/* 0x80 */ "Reset Complete.",
	/* 0x81 */ "Message queue overflow detected.",
	/* 0x82 */ "Overcurrent Detected",
	/* 0x83 */ "Overvoltage Detected",
	/* 0x84 */ "Type-C Port Connect Detected",
	/* 0x85 */ "Type-C Port Disconnect Detected",
	/* 0x86 */ "PD Contract Negotiation Complete",
	/* 0x87 */ "SWAP Complete",
	/* 0x88 */ "0x88",
	/* 0x89 */ "0x89",
	/* 0x8A */ "PS_RDY Message Received",
	/* 0x8B */ "GotoMin Message Received.",
	/* 0x8C */ "Accept Message Received",
	/* 0x8D */ "Reject Message Received",
	/* 0x8E */ "Wait Message Received",
	/* 0x8F */ "Hard Reset Received",
	/* 0x90 */ "VDM Received",
	/* 0x91 */ "Source Capabilities Message Received",
	/* 0x92 */ "Sink Capabilities Message Received",
	/* 0x93 */ "Display Port Alternate Mode entered",
	/* 0x94 */ "Display Port device connected at UFP_U",
	/* 0x95 */ "Display port device not connected at UFP_U",
	/* 0x96 */ "Display port SID not found in Discover SID process",
	/* 0x97 */ "Multiple SVIDs discovered along with DisplayPort SID",
	/* 0x98 */ "DP Functionality not supported by Cable",
	/* 0x99 */ "Display Port Configuration not supported by UFP",
	/* 0x9A */ "Hard Reset Sent to Port Partner",
	/* 0x9B */ "Soft Reset Sent to Port Partner",
	/* 0x9C */ "Cable Reset Sent to EMCA",
	/* 0x9D */ "Source Disabled State Entered",
	/* 0x9E */ "Sender Response Timer Timeout",
	/* 0x9F */ "No VDM Response Received",
	/* 0xA0 */ "Unexpected Voltage on Vbus",
	/* 0xA1 */ "Type-C Error Recovery",
	/* 0xA2 */ "0xA2",
	/* 0xA3 */ "0xA3",
	/* 0xA4 */ "0xA4",
	/* 0xA5 */ "0xA5",
	/* 0xA6 */ "EMCA Detected",
	/* 0xA7 */ "0xA7",
	/* 0xA8 */ "0xA8",
	/* 0xA9 */ "0xA9",
	/* 0xAA */ "Rp Change Detected",
};

struct ccg_cmd {
	u16 reg;
	u32 data;
	int len;
	int delay; /* ms delay for cmd timeout  */
};

struct ccg_resp {
	u8 code;
	u8 length;
};

struct ucsi_ccg {
	struct device *dev;
	struct ucsi *ucsi;
	struct ucsi_ppm ppm;
	struct i2c_client *client;
	struct ccg_dev_info info;
	/* CCG HPI communication flags */
	unsigned long flags;
#define RESET_PENDING	0
#define DEV_CMD_PENDING	1
	struct ccg_resp dev_resp;
	u8 cmd_resp;
	int port_num;
	struct mutex lock; /* to sync between user and driver thread */
};

static int ccg_read(struct ucsi_ccg *uc, u16 rab, u8 *data, u32 len)
{
	struct i2c_client *client = uc->client;
	const struct i2c_adapter_quirks *quirks = client->adapter->quirks;
	unsigned char buf[2];
	struct i2c_msg msgs[] = {
		{
			.addr	= client->addr,
			.flags  = 0x0,
			.len	= sizeof(buf),
			.buf	= buf,
		},
		{
			.addr	= client->addr,
			.flags  = I2C_M_RD,
			.buf	= data,
		},
	};
	u32 rlen, rem_len = len, max_read_len = len;
	int status;

	/* check any max_read_len limitation on i2c adapter */
	if (quirks && quirks->max_read_len)
		max_read_len = quirks->max_read_len;

	while (rem_len > 0) {
		msgs[1].buf = &data[len - rem_len];
		rlen = min_t(u16, rem_len, max_read_len);
		msgs[1].len = rlen;
		put_unaligned_le16(rab, buf);
		status = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
		if (status < 0) {
			dev_err(uc->dev, "i2c_transfer failed %d\n", status);
			return status;
		}
		rab += rlen;
		rem_len -= rlen;
	}

	return 0;
}

static int ccg_write(struct ucsi_ccg *uc, u16 rab, u8 *data, u32 len)
{
	struct i2c_client *client = uc->client;
	unsigned char *buf;
	struct i2c_msg msgs[] = {
		{
			.addr	= client->addr,
			.flags  = 0x0,
		}
	};
	int status;

	buf = kzalloc(len + sizeof(rab), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	put_unaligned_le16(rab, buf);
	memcpy(buf + sizeof(rab), data, len);

	msgs[0].len = len + sizeof(rab);
	msgs[0].buf = buf;

	status = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (status < 0) {
		dev_err(uc->dev, "i2c_transfer failed %d\n", status);
		kfree(buf);
		return status;
	}

	kfree(buf);
	return 0;
}

static int ucsi_ccg_init(struct ucsi_ccg *uc)
{
	unsigned int count = 10;
	u8 data;
	int status;

	data = CCGX_RAB_UCSI_CONTROL_STOP;
	status = ccg_write(uc, CCGX_RAB_UCSI_CONTROL, &data, sizeof(data));
	if (status < 0)
		return status;

	data = CCGX_RAB_UCSI_CONTROL_START;
	status = ccg_write(uc, CCGX_RAB_UCSI_CONTROL, &data, sizeof(data));
	if (status < 0)
		return status;

	/*
	 * Flush CCGx RESPONSE queue by acking interrupts. Above ucsi control
	 * register write will push response which must be cleared.
	 */
	do {
		status = ccg_read(uc, CCGX_RAB_INTR_REG, &data, sizeof(data));
		if (status < 0)
			return status;

		if (!data)
			return 0;

		status = ccg_write(uc, CCGX_RAB_INTR_REG, &data, sizeof(data));
		if (status < 0)
			return status;

		usleep_range(10000, 11000);
	} while (--count);

	return -ETIMEDOUT;
}

static int ucsi_ccg_send_data(struct ucsi_ccg *uc)
{
	u8 *ppm = (u8 *)uc->ppm.data;
	int status;
	u16 rab;

	rab = CCGX_RAB_UCSI_DATA_BLOCK(offsetof(struct ucsi_data, message_out));
	status = ccg_write(uc, rab, ppm +
			   offsetof(struct ucsi_data, message_out),
			   sizeof(uc->ppm.data->message_out));
	if (status < 0)
		return status;

	rab = CCGX_RAB_UCSI_DATA_BLOCK(offsetof(struct ucsi_data, ctrl));
	return ccg_write(uc, rab, ppm + offsetof(struct ucsi_data, ctrl),
			 sizeof(uc->ppm.data->ctrl));
}

static int ucsi_ccg_recv_data(struct ucsi_ccg *uc)
{
	u8 *ppm = (u8 *)uc->ppm.data;
	int status;
	u16 rab;

	rab = CCGX_RAB_UCSI_DATA_BLOCK(offsetof(struct ucsi_data, cci));
	status = ccg_read(uc, rab, ppm + offsetof(struct ucsi_data, cci),
			  sizeof(uc->ppm.data->cci));
	if (status < 0)
		return status;

	rab = CCGX_RAB_UCSI_DATA_BLOCK(offsetof(struct ucsi_data, message_in));
	return ccg_read(uc, rab, ppm + offsetof(struct ucsi_data, message_in),
			sizeof(uc->ppm.data->message_in));
}

static int ucsi_ccg_ack_interrupt(struct ucsi_ccg *uc)
{
	int status;
	unsigned char data;

	status = ccg_read(uc, CCGX_RAB_INTR_REG, &data, sizeof(data));
	if (status < 0)
		return status;

	return ccg_write(uc, CCGX_RAB_INTR_REG, &data, sizeof(data));
}

static int ucsi_ccg_sync(struct ucsi_ppm *ppm)
{
	struct ucsi_ccg *uc = container_of(ppm, struct ucsi_ccg, ppm);
	int status;

	status = ucsi_ccg_recv_data(uc);
	if (status < 0)
		return status;

	/* ack interrupt to allow next command to run */
	return ucsi_ccg_ack_interrupt(uc);
}

static int ucsi_ccg_cmd(struct ucsi_ppm *ppm, struct ucsi_control *ctrl)
{
	struct ucsi_ccg *uc = container_of(ppm, struct ucsi_ccg, ppm);

	ppm->data->ctrl.raw_cmd = ctrl->raw_cmd;
	return ucsi_ccg_send_data(uc);
}

static irqreturn_t ccg_irq_handler(int irq, void *data)
{
	struct ucsi_ccg *uc = data;

	ucsi_notify(uc->ucsi);

	return IRQ_HANDLED;
}

static int get_fw_info(struct ucsi_ccg *uc)
{
	struct device *dev = uc->dev;
	struct version_info version[3];
	struct version_info *v;
	int err, i;

	err = ccg_read(uc, CCGX_RAB_READ_ALL_VER, (u8 *)(&version),
		       sizeof(version));
	if (err < 0)
		return err;

	for (i = 1; i < ARRAY_SIZE(version); i++) {
		v = &version[i];
		dev_dbg(dev,
			"FW%d Version: %c%c v%x.%x%x, [Base %d.%d.%d.%d]\n",
			i, (v->app.build >> 8), (v->app.build & 0xFF),
			v->app.patch, v->app.maj, v->app.min,
			v->base.maj, v->base.min, v->base.patch,
			v->base.build);
	}

	err = ccg_read(uc, CCGX_RAB_DEVICE_MODE, (u8 *)(&uc->info),
		       sizeof(uc->info));
	if (err < 0)
		return err;

	dev_dbg(dev, "fw_mode: %d\n", uc->info.fw_mode);
	dev_dbg(dev, "fw1_invalid: %d\n", uc->info.fw1_invalid);
	dev_dbg(dev, "fw2_invalid: %d\n", uc->info.fw2_invalid);
	dev_dbg(dev, "silicon_id: 0x%04x\n", uc->info.silicon_id);

	return 0;
}

static inline bool invalid_resp(int code)
{
	return (code >= INVALID_RESP);
}

static inline bool invalid_evt(int code)
{
	unsigned long num_of_events = ARRAY_SIZE(ccg_evt_strs);

	return (code >= (EVENT_INDEX + num_of_events)) || (code < EVENT_INDEX);
}

static void ccg_process_response(struct ucsi_ccg *uc)
{
	struct device *dev = uc->dev;

	if (uc->dev_resp.code & ASYNC_EVENT) {
		if (uc->dev_resp.code == RESET_COMPLETE) {
			if (test_bit(RESET_PENDING, &uc->flags))
				uc->cmd_resp = uc->dev_resp.code;
			dev_info(dev, "CCG reset complete\n");
			get_fw_info(uc);
		}

		if (!invalid_evt(uc->dev_resp.code))
			dev_dbg(dev, "%s\n",
				ccg_evt_strs[uc->dev_resp.code - EVENT_INDEX]);
		else
			dev_err(dev, "invalid evt %d\n", uc->dev_resp.code);
	} else {
		if (test_bit(DEV_CMD_PENDING, &uc->flags)) {
			uc->cmd_resp = uc->dev_resp.code;
			clear_bit(DEV_CMD_PENDING, &uc->flags);
		} else {
			dev_err(dev, "dev resp 0x%04x but no cmd pending\n",
				uc->dev_resp.code);
		}
	}
}

static int ccg_read_response(struct ucsi_ccg *uc)
{
	unsigned long target = jiffies + msecs_to_jiffies(1000);
	struct device *dev = uc->dev;
	u8 intval;
	int status;

	/* wait for interrupt status to get updated */
	do {
		status = ccg_read(uc, CCGX_RAB_INTR_REG, &intval,
				  sizeof(intval));
		if (status < 0)
			return status;

		if (intval & DEV_INT)
			break;
		usleep_range(500, 600);
	} while (time_is_after_jiffies(target));

	if (time_is_before_jiffies(target)) {
		dev_err(dev, "response timeout error\n");
		return -ETIME;
	}

	status = ccg_read(uc, CCGX_RAB_RESPONSE, (u8 *)&uc->dev_resp,
			  sizeof(uc->dev_resp));
	if (status < 0)
		return status;

	dev_dbg(dev, "dev event code: 0x%02x, data len: %d\n",
		uc->dev_resp.code, uc->dev_resp.length);

	status = ccg_write(uc, CCGX_RAB_INTR_REG, &intval, sizeof(intval));
	if (status < 0)
		return status;

	return 0;
}

/* Caller must hold uc->lock */
static int ccg_send_command(struct ucsi_ccg *uc, struct ccg_cmd *cmd)
{
	struct device *dev = uc->dev;
	int ret;

	switch (cmd->reg & 0xF000) {
	case DEV_REG_IDX:
		set_bit(DEV_CMD_PENDING, &uc->flags);
		break;
	default:
		dev_err(dev, "invalid cmd register\n");
		break;
	}

	ret = ccg_write(uc, cmd->reg, (u8 *)&cmd->data, cmd->len);
	if (ret < 0)
		return ret;

	dev_dbg(dev, "reg=0x%04x data=0x%08x delay=%d\n",
		cmd->reg, cmd->data, cmd->delay);

	msleep(cmd->delay);

	ret = ccg_read_response(uc);
	if (ret < 0) {
		dev_err(dev, "response read error\n");
		switch (cmd->reg & 0xF000) {
		case DEV_REG_IDX:
			clear_bit(DEV_CMD_PENDING, &uc->flags);
			break;
		default:
			dev_err(dev, "invalid cmd register\n");
			break;
		}
		return -EIO;
	}
	ccg_process_response(uc);

	return uc->cmd_resp;
}

static int ccg_cmd_enter_flashing(struct ucsi_ccg *uc)
{
	struct ccg_cmd cmd;
	int ret;

	cmd.reg = CCGX_RAB_ENTER_FLASHING;
	cmd.data = FLASH_ENTER_SIG;
	cmd.len = 1;
	cmd.delay = 50;

	mutex_lock(&uc->lock);

	ret = ccg_send_command(uc, &cmd);

	mutex_unlock(&uc->lock);

	if (ret != CMD_SUCCESS) {
		dev_err(uc->dev, "enter flashing failed ret=%d\n", ret);
		return ret;
	}

	return 0;
}

static int ccg_cmd_reset(struct ucsi_ccg *uc, bool extra_delay)
{
	struct ccg_cmd cmd;
	u8 *p;
	int ret;

	p = (u8 *)&cmd.data;
	cmd.reg = CCGX_RAB_RESET_REQ;
	p[0] = RESET_SIG;
	p[1] = CMD_RESET_DEV;
	cmd.len = 2;
	cmd.delay = 2000 + (extra_delay ? 3000 : 0);

	mutex_lock(&uc->lock);

	set_bit(RESET_PENDING, &uc->flags);

	ret = ccg_send_command(uc, &cmd);
	if (ret != RESET_COMPLETE)
		goto err_clear_flag;

	ret = 0;

err_clear_flag:
	clear_bit(RESET_PENDING, &uc->flags);

	mutex_unlock(&uc->lock);

	return ret;
}

static int ccg_cmd_port_control(struct ucsi_ccg *uc, bool enable)
{
	struct ccg_cmd cmd;
	int ret;

	cmd.reg = CCGX_RAB_PDPORT_ENABLE;
	if (enable)
		cmd.data = (uc->port_num == 1) ?
			    PDPORT_1 : (PDPORT_1 | PDPORT_2);
	else
		cmd.data = 0x0;
	cmd.len = 1;
	cmd.delay = 10;

	mutex_lock(&uc->lock);

	ret = ccg_send_command(uc, &cmd);

	mutex_unlock(&uc->lock);

	if (ret != CMD_SUCCESS) {
		dev_err(uc->dev, "port control failed ret=%d\n", ret);
		return ret;
	}
	return 0;
}

static int ccg_cmd_jump_boot_mode(struct ucsi_ccg *uc, int bl_mode)
{
	struct ccg_cmd cmd;
	int ret;

	cmd.reg = CCGX_RAB_JUMP_TO_BOOT;

	if (bl_mode)
		cmd.data = TO_BOOT;
	else
		cmd.data = TO_ALT_FW;

	cmd.len = 1;
	cmd.delay = 100;

	mutex_lock(&uc->lock);

	set_bit(RESET_PENDING, &uc->flags);

	ret = ccg_send_command(uc, &cmd);
	if (ret != RESET_COMPLETE)
		goto err_clear_flag;

	ret = 0;

err_clear_flag:
	clear_bit(RESET_PENDING, &uc->flags);

	mutex_unlock(&uc->lock);

	return ret;
}

static int
ccg_cmd_write_flash_row(struct ucsi_ccg *uc, u16 row,
			const void *data, u8 fcmd)
{
	struct i2c_client *client = uc->client;
	struct ccg_cmd cmd;
	u8 buf[CCG4_ROW_SIZE + 2];
	u8 *p;
	int ret;

	/* Copy the data into the flash read/write memory. */
	buf[0] = REG_FLASH_RW_MEM & 0xFF;
	buf[1] = REG_FLASH_RW_MEM >> 8;

	memcpy(buf + 2, data, CCG4_ROW_SIZE);

	mutex_lock(&uc->lock);

	ret = i2c_master_send(client, buf, CCG4_ROW_SIZE + 2);
	if (ret != CCG4_ROW_SIZE + 2) {
		dev_err(uc->dev, "REG_FLASH_RW_MEM write fail %d\n", ret);
		return ret < 0 ? ret : -EIO;
	}

	/* Use the FLASH_ROW_READ_WRITE register to trigger */
	/* writing of data to the desired flash row */
	p = (u8 *)&cmd.data;
	cmd.reg = CCGX_RAB_FLASH_ROW_RW;
	p[0] = FLASH_SIG;
	p[1] = fcmd;
	p[2] = row & 0xFF;
	p[3] = row >> 8;
	cmd.len = 4;
	cmd.delay = 50;
	if (fcmd == FLASH_FWCT_SIG_WR_CMD)
		cmd.delay += 400;
	if (row == 510)
		cmd.delay += 220;
	ret = ccg_send_command(uc, &cmd);

	mutex_unlock(&uc->lock);

	if (ret != CMD_SUCCESS) {
		dev_err(uc->dev, "write flash row failed ret=%d\n", ret);
		return ret;
	}

	return 0;
}

static int ccg_cmd_validate_fw(struct ucsi_ccg *uc, unsigned int fwid)
{
	struct ccg_cmd cmd;
	int ret;

	cmd.reg = CCGX_RAB_VALIDATE_FW;
	cmd.data = fwid;
	cmd.len = 1;
	cmd.delay = 500;

	mutex_lock(&uc->lock);

	ret = ccg_send_command(uc, &cmd);

	mutex_unlock(&uc->lock);

	if (ret != CMD_SUCCESS)
		return ret;

	return 0;
}

static bool ccg_check_fw_version(struct ucsi_ccg *uc, const char *fw_name,
				 struct version_format *app)
{
	const struct firmware *fw = NULL;
	struct device *dev = uc->dev;
	struct fw_config_table fw_cfg;
	u32 cur_version, new_version;
	bool is_later = false;

	if (request_firmware(&fw, fw_name, dev) != 0) {
		dev_err(dev, "error: Failed to open cyacd file %s\n", fw_name);
		return false;
	}

	/*
	 * check if signed fw
	 * last part of fw image is fw cfg table and signature
	 */
	if (fw->size < sizeof(fw_cfg) + FW_CFG_TABLE_SIG_SIZE)
		goto not_signed_fw;

	memcpy((uint8_t *)&fw_cfg, fw->data + fw->size -
	       sizeof(fw_cfg) - FW_CFG_TABLE_SIG_SIZE, sizeof(fw_cfg));

	if (fw_cfg.identity != ('F' | ('W' << 8) | ('C' << 16) | ('T' << 24))) {
		dev_info(dev, "not a signed image\n");
		goto not_signed_fw;
	}

	/* compare input version with FWCT version */
	cur_version = app->build | (app->patch << 16) |
			((app->min | (app->maj << 4)) << 24);

	new_version = fw_cfg.app.build | (fw_cfg.app.patch << 16) |
			((fw_cfg.app.min | (fw_cfg.app.maj << 4)) << 24);

	dev_dbg(dev, "compare current %08x and new version %08x\n",
		cur_version, new_version);

	if (new_version > cur_version) {
		dev_dbg(dev, "new firmware file version is later\n");
		is_later = true;
	} else {
		dev_dbg(dev, "new firmware file version is same or earlier\n");
	}

not_signed_fw:
	release_firmware(fw);
	return is_later;
}

static int ccg_fw_update_needed(struct ucsi_ccg *uc,
				enum enum_flash_mode *mode)
{
	struct device *dev = uc->dev;
	int err;
	struct version_info version[3];

	err = ccg_read(uc, CCGX_RAB_DEVICE_MODE, (u8 *)(&uc->info),
		       sizeof(uc->info));
	if (err) {
		dev_err(dev, "read device mode failed\n");
		return err;
	}

	err = ccg_read(uc, CCGX_RAB_READ_ALL_VER, (u8 *)version,
		       sizeof(version));
	if (err) {
		dev_err(dev, "read device mode failed\n");
		return err;
	}

	dev_dbg(dev, "check if fw upgrade required %x %x %x %x %x %x %x %x\n",
		version[FW1].base.build, version[FW1].base.patch,
		version[FW1].base.min, version[FW1].base.maj,
		version[FW2].app.build, version[FW2].app.patch,
		version[FW2].app.min, version[FW2].app.maj);

	if (memcmp(&version[FW1], "\0\0\0\0\0\0\0\0",
		   sizeof(struct version_info)) == 0) {
		dev_info(dev, "secondary fw is not flashed\n");
		*mode = SECONDARY_BL;
	} else if (version[FW1].base.build < secondary_fw_min_ver) {
		dev_info(dev, "secondary fw version is too low (< %d)\n",
			 secondary_fw_min_ver);
		*mode = SECONDARY;
	} else if (memcmp(&version[FW2], "\0\0\0\0\0\0\0\0",
		   sizeof(struct version_info)) == 0) {
		dev_info(dev, "primary fw is not flashed\n");
		*mode = PRIMARY;
	} else if (ccg_check_fw_version(uc, ccg_fw_names[PRIMARY],
		   &version[FW2].app)) {
		dev_info(dev, "found primary fw with later version\n");
		*mode = PRIMARY;
	} else {
		dev_info(dev, "secondary and primary fw are the latest\n");
		*mode = FLASH_NOT_NEEDED;
	}
	return 0;
}

static int ucsi_ccg_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	struct device *dev = &client->dev;
	struct ucsi_ccg *uc;
	int status;
	u16 rab;

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

	status = get_fw_info(uc);
	if (status < 0) {
		dev_err(uc->dev, "get_fw_info failed - %d\n", status);
		return status;
	}

	if (uc->info.two_pd_ports)
		uc->port_num = 2;
	else
		uc->port_num = 1;

	status = devm_request_threaded_irq(dev, client->irq, NULL,
					   ccg_irq_handler,
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

	rab = CCGX_RAB_UCSI_DATA_BLOCK(offsetof(struct ucsi_data, version));
	status = ccg_read(uc, rab, (u8 *)(uc->ppm.data) +
			  offsetof(struct ucsi_data, version),
			  sizeof(uc->ppm.data->version));
	if (status < 0) {
		ucsi_unregister_ppm(uc->ucsi);
		return status;
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
	{}
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
