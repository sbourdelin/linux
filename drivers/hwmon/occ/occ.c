/*
 * occ.c - OCC hwmon driver
 *
 * This file contains the methods and data structures for the OCC hwmon driver.
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

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/err.h>
#include <linux/mutex.h>
#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <asm/unaligned.h>

#include "occ.h"

#define OCC_DATA_MAX		4096
#define OCC_BMC_TIMEOUT_MS	20000

/* To generate attn to OCC */
#define ATTN_DATA		0x0006B035

/* For BMC to read/write SRAM */
#define OCB_ADDRESS		0x0006B070
#define OCB_DATA		0x0006B075
#define OCB_STATUS_CONTROL_AND	0x0006B072
#define OCB_STATUS_CONTROL_OR	0x0006B073

/* To init OCB */
#define OCB_AND_INIT0		0xFBFFFFFF
#define OCB_AND_INIT1		0xFFFFFFFF
#define OCB_OR_INIT0		0x08000000
#define OCB_OR_INIT1		0x00000000

/* To generate attention on OCC */
#define ATTN0			0x01010000
#define ATTN1			0x00000000

/* OCC return status */
#define RESP_RETURN_CMD_IN_PRG	0xFF
#define RESP_RETURN_SUCCESS	0
#define RESP_RETURN_CMD_INVAL	0x11
#define RESP_RETURN_CMD_LEN	0x12
#define RESP_RETURN_DATA_INVAL	0x13
#define RESP_RETURN_CHKSUM	0x14
#define RESP_RETURN_OCC_ERR	0x15
#define RESP_RETURN_STATE	0x16

/* time interval to retry on "command in progress" return status */
#define CMD_IN_PRG_INT_MS	100
#define CMD_IN_PRG_RETRIES	(OCC_BMC_TIMEOUT_MS / CMD_IN_PRG_INT_MS)

/* OCC command definitions */
#define OCC_POLL		0
#define OCC_SET_USER_POWR_CAP	0x22

/* OCC poll command data */
#define OCC_POLL_STAT_SENSOR	0x10

/* OCC response data offsets */
#define RESP_RETURN_STATUS	2
#define RESP_DATA_LENGTH	3
#define RESP_HEADER_OFFSET	5
#define SENSOR_STR_OFFSET	37
#define SENSOR_BLOCK_NUM_OFFSET	43
#define SENSOR_BLOCK_OFFSET	45

/* occ_poll_header
 * structure to match the raw occ poll response data
 */
struct occ_poll_header {
	u8 status;
	u8 ext_status;
	u8 occs_present;
	u8 config;
	u8 occ_state;
	u8 mode;
	u8 ips_status;
	u8 error_log_id;
	u32 error_log_addr_start;
	u16 error_log_length;
	u8 reserved2;
	u8 reserved3;
	u8 occ_code_level[16];
	u8 sensor_eye_catcher[6];
	u8 sensor_block_num;
	u8 sensor_data_version;
} __attribute__((packed, aligned(4)));

struct occ_response {
	struct occ_poll_header header;
	struct occ_blocks data;
};

struct occ {
	struct device *dev;
	void *bus;
	struct occ_bus_ops bus_ops;
	struct occ_ops ops;
	struct occ_config config;
	unsigned long update_interval;
	unsigned long last_updated;
	struct mutex update_lock;
	struct occ_response response;
	bool valid;
};

static void deinit_occ_resp_buf(struct occ_response *resp)
{
	int i;

	if (!resp)
		return;

	if (!resp->data.blocks)
		return;

	for (i = 0; i < resp->header.sensor_block_num; ++i)
		kfree(resp->data.blocks[i].sensors);

	kfree(resp->data.blocks);

	memset(resp, 0, sizeof(struct occ_response));

	for (i = 0; i < MAX_OCC_SENSOR_TYPE; ++i)
		resp->data.sensor_block_id[i] = -1;
}

static void *occ_get_sensor_by_type(struct occ_response *resp,
				    enum sensor_type t)
{
	if (!resp->data.blocks)
		return NULL;

	if (resp->data.sensor_block_id[t] == -1)
		return NULL;

	return resp->data.blocks[resp->data.sensor_block_id[t]].sensors;
}

static int occ_check_sensor(struct occ *driver, u8 sensor_length,
			    u8 sensor_num, enum sensor_type t, int block)
{
	void *sensor;
	int type_block_id;
	struct occ_response *resp = &driver->response;

	sensor = occ_get_sensor_by_type(resp, t);

	/* empty sensor block, release older sensor data */
	if (sensor_num == 0 || sensor_length == 0) {
		kfree(sensor);
		dev_err(driver->dev, "no sensor blocks available\n");
		return -ENODATA;
	}

	type_block_id = resp->data.sensor_block_id[t];
	if (!sensor || sensor_num !=
	    resp->data.blocks[type_block_id].header.sensor_num) {
		kfree(sensor);
		resp->data.blocks[block].sensors =
			driver->ops.alloc_sensor(t, sensor_num);
		if (!resp->data.blocks[block].sensors)
			return -ENOMEM;
	}

	return 0;
}

static int parse_occ_response(struct occ *driver, u8 *data,
			      struct occ_response *resp)
{
	int b;
	int s;
	int rc;
	int offset = SENSOR_BLOCK_OFFSET;
	int sensor_type;
	u8 sensor_block_num;
	char sensor_type_string[5] = { 0 };
	struct sensor_data_block_header *block;
	struct device *dev = driver->dev;

	/* check if the data is valid */
	if (strncmp(&data[SENSOR_STR_OFFSET], "SENSOR", 6) != 0) {
		dev_err(dev, "no SENSOR string in response\n");
		rc = -ENODATA;
		goto err;
	}

	sensor_block_num = data[SENSOR_BLOCK_NUM_OFFSET];
	if (sensor_block_num == 0) {
		dev_err(dev, "no sensor blocks available\n");
		rc = -ENODATA;
		goto err;
	}

	/* if number of sensor block has changed, re-malloc */
	if (sensor_block_num != resp->header.sensor_block_num) {
		deinit_occ_resp_buf(resp);
		resp->data.blocks = kcalloc(sensor_block_num,
					    sizeof(struct sensor_data_block),
					    GFP_KERNEL);
		if (!resp->data.blocks)
			return -ENOMEM;
	}

	memcpy(&resp->header, &data[RESP_HEADER_OFFSET],
	       sizeof(struct occ_poll_header));
	resp->header.error_log_addr_start =
		be32_to_cpu(resp->header.error_log_addr_start);
	resp->header.error_log_length =
		be16_to_cpu(resp->header.error_log_length);

	dev_dbg(dev, "Reading %d sensor blocks\n",
		resp->header.sensor_block_num);
	for (b = 0; b < sensor_block_num; b++) {
		block = (struct sensor_data_block_header *)&data[offset];
		/* copy to a null terminated string */
		strncpy(sensor_type_string, block->sensor_type, 4);
		offset += 8;

		dev_dbg(dev, "sensor block[%d]: type: %s, sensor_num: %d\n", b,
			sensor_type_string, block->sensor_num);

		if (strncmp(block->sensor_type, "FREQ", 4) == 0)
			sensor_type = FREQ;
		else if (strncmp(block->sensor_type, "TEMP", 4) == 0)
			sensor_type = TEMP;
		else if (strncmp(block->sensor_type, "POWR", 4) == 0)
			sensor_type = POWER;
		else if (strncmp(block->sensor_type, "CAPS", 4) == 0)
			sensor_type = CAPS;
		else {
			dev_err(dev, "sensor type not supported %s\n",
				sensor_type_string);
			continue;
		}

		rc = occ_check_sensor(driver, block->sensor_length,
				      block->sensor_num, sensor_type, b);
		if (rc == -ENOMEM)
			goto err;
		else if (rc)
			continue;

		resp->data.sensor_block_id[sensor_type] = b;
		for (s = 0; s < block->sensor_num; s++) {
			driver->ops.parse_sensor(data,
						 resp->data.blocks[b].sensors,
						 sensor_type, offset, s);
			offset += block->sensor_length;
		}

		/* copy block data over to response pointer */
		resp->data.blocks[b].header = *block;
	}

	return 0;
err:
	deinit_occ_resp_buf(resp);
	return rc;
}

static u8 occ_send_cmd(struct occ *driver, u8 seq, u8 type, u16 length,
		       u8 *data, u8 *resp)
{
	u32 cmd1, cmd2;
	u16 checksum = 0;
	u16 length_le = cpu_to_le16(length);
	bool retry = 0;
	int i, rc, tries = 0;

	cmd1 = (seq << 24) | (type << 16) | length_le;
	memcpy(&cmd2, data, length);
	cmd2 <<= ((4 - length) * 8);

	/* checksum: sum of every bytes of cmd1, cmd2 */
	for (i = 0; i < 4; i++) {
		checksum += (cmd1 >> (i * 8)) & 0xFF;
		checksum += (cmd2 >> (i * 8)) & 0xFF;
	}

	cmd2 |= checksum << ((2 - length) * 8);

	/* Init OCB */
	rc = driver->bus_ops.putscom(driver->bus, OCB_STATUS_CONTROL_OR,
				     OCB_OR_INIT0, OCB_OR_INIT1);
	if (rc)
		goto err;

	rc = driver->bus_ops.putscom(driver->bus, OCB_STATUS_CONTROL_AND,
				     OCB_AND_INIT0, OCB_AND_INIT1);
	if (rc)
		goto err;

	/* Send command, 2nd half of the 64-bit addr is unused (write 0) */
	rc = driver->bus_ops.putscom(driver->bus, OCB_ADDRESS,
				     driver->config.command_addr, 0);
	if (rc)
		goto err;

	rc = driver->bus_ops.putscom(driver->bus, OCB_DATA, cmd1, cmd2);
	if (rc)
		goto err;

	/* Trigger attention */
	rc = driver->bus_ops.putscom(driver->bus, ATTN_DATA, ATTN0, ATTN1);
	if (rc)
		goto err;

	/* Get response data */
	rc = driver->bus_ops.putscom(driver->bus, OCB_ADDRESS,
				     driver->config.response_addr, 0);
	if (rc)
		goto err;

	do {
		if (retry) {
			set_current_state(TASK_INTERRUPTIBLE);
			schedule_timeout(msecs_to_jiffies(CMD_IN_PRG_INT_MS));
		}

		rc = driver->bus_ops.getscom(driver->bus, OCB_DATA,
					     (u64 *)resp);
		if (rc)
			goto err;

		/* retry if we get "command in progress" return status */
		retry = (resp[RESP_RETURN_STATUS] == RESP_RETURN_CMD_IN_PRG) &&
			(tries++ < CMD_IN_PRG_RETRIES);
	} while (retry);

	switch (resp[RESP_RETURN_STATUS]) {
	case RESP_RETURN_CMD_IN_PRG:
		rc = -EALREADY;
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

	return rc;

err:
	dev_err(driver->dev, "scom op failed rc:%d\n", rc);
	return rc;
}

static int occ_get_all(struct occ *driver)
{
	int i = 0, rc;
	u8 *occ_data;
	u16 num_bytes;
	const u8 poll_cmd_data = OCC_POLL_STAT_SENSOR;
	struct device *dev = driver->dev;
	struct occ_response *resp = &driver->response;

	occ_data = devm_kzalloc(dev, OCC_DATA_MAX, GFP_KERNEL);
	if (!occ_data)
		return -ENOMEM;

	rc = occ_send_cmd(driver, 0, OCC_POLL, 1, &poll_cmd_data, occ_data);
	if (rc) {
		dev_err(dev, "OCC poll failed: %d\n", rc);
		goto out;
	}

	num_bytes = get_unaligned((u16 *)&occ_data[RESP_DATA_LENGTH]);
	num_bytes = be16_to_cpu(num_bytes);
	dev_dbg(dev, "OCC data length: %d\n", num_bytes);

	if (num_bytes > OCC_DATA_MAX) {
		dev_err(dev, "OCC data length must be < 4KB\n");
		rc = -EINVAL;
		goto out;
	}

	if (num_bytes <= 0) {
		dev_err(dev, "OCC data length is zero\n");
		rc = -EINVAL;
		goto out;
	}

	/* read remaining data */
	for (i = 8; i < num_bytes + 8; i += 8) {
		rc = driver->bus_ops.getscom(driver->bus, OCB_DATA,
					     (u64 *)&occ_data[i]);
		if (rc) {
			dev_err(dev, "scom op failed rc:%d\n", rc);
			goto out;
		}
	}

	/* don't need more sanity checks; buffer is alloc'd for max response
	 * size so we just check for valid data in parse_occ_response
	 */
	rc = parse_occ_response(driver, occ_data, resp);

out:
	devm_kfree(dev, occ_data);
	return rc;
}

int occ_update_device(struct occ *driver)
{
	int rc = 0;

	mutex_lock(&driver->update_lock);

	if (time_after(jiffies, driver->last_updated + driver->update_interval)
	    || !driver->valid) {
		driver->valid = 1;

		rc = occ_get_all(driver);
		if (rc)
			driver->valid = 0;

		driver->last_updated = jiffies;
	}

	mutex_unlock(&driver->update_lock);

	return rc;
}
EXPORT_SYMBOL(occ_update_device);

void *occ_get_sensor(struct occ *driver, int sensor_type)
{
	int rc;

	/* occ_update_device locks the update lock */
	rc = occ_update_device(driver);
	if (rc != 0) {
		dev_err(driver->dev, "cannot get occ sensor data: %d\n",
			rc);
		return NULL;
	}

	return occ_get_sensor_by_type(&driver->response, sensor_type);
}
EXPORT_SYMBOL(occ_get_sensor);

int occ_get_sensor_value(struct occ *occ, int sensor_type, int snum)
{
	return occ->ops.get_sensor_value(occ, sensor_type, snum);
}
EXPORT_SYMBOL(occ_get_sensor_value);

int occ_get_sensor_id(struct occ *occ, int sensor_type, int snum)
{
	return occ->ops.get_sensor_id(occ, sensor_type, snum);
}
EXPORT_SYMBOL(occ_get_sensor_id);

int occ_get_caps_value(struct occ *occ, void *sensor, int snum, int caps_field)
{
	return occ->ops.get_caps_value(sensor, snum, caps_field);
}
EXPORT_SYMBOL(occ_get_caps_value);

void occ_get_response_blocks(struct occ *occ, struct occ_blocks **blocks)
{
	*blocks = &occ->response.data;
}
EXPORT_SYMBOL(occ_get_response_blocks);

void occ_set_update_interval(struct occ *occ, unsigned long interval)
{
	occ->update_interval = msecs_to_jiffies(interval);
}
EXPORT_SYMBOL(occ_set_update_interval);

int occ_set_user_powercap(struct occ *occ, u16 cap)
{
	u8 resp[8];

	cap = cpu_to_be16(cap);

	return occ_send_cmd(occ, 0, OCC_SET_USER_POWR_CAP, 2, (u8 *)&cap,
			    resp);
}
EXPORT_SYMBOL(occ_set_user_powercap);

struct occ *occ_start(struct device *dev, void *bus,
		      struct occ_bus_ops *bus_ops, const struct occ_ops *ops,
		      const struct occ_config *config)
{
	struct occ *driver = devm_kzalloc(dev, sizeof(struct occ), GFP_KERNEL);

	if (!driver)
		return ERR_PTR(-ENOMEM);

	driver->dev = dev;
	driver->bus = bus;
	driver->bus_ops = *bus_ops;
	driver->ops = *ops;
	driver->config = *config;

	driver->update_interval = HZ;
	mutex_init(&driver->update_lock);

	return driver;
}
EXPORT_SYMBOL(occ_start);

int occ_stop(struct occ *occ)
{
	devm_kfree(occ->dev, occ);

	return 0;
}
EXPORT_SYMBOL(occ_stop);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("OCC hwmon core driver");
MODULE_LICENSE("GPL");
