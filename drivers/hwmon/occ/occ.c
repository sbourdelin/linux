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

#include <asm/unaligned.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "occ.h"

#define OCC_DATA_MAX		4096
#define OCC_DATA_MIN		40
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
#define RESP_RETURN_STATUS		2
#define RESP_DATA_LENGTH		3
#define RESP_HEADER_OFFSET		5
#define SENSOR_STR_OFFSET		37
#define NUM_SENSOR_BLOCKS_OFFSET	43
#define SENSOR_BLOCK_OFFSET		45

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
	u8 num_sensor_blocks;
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
	unsigned long last_updated;
	struct mutex update_lock;
	struct occ_response response;
	bool valid;
	u8 *raw_data;
};

static int parse_occ_response(struct occ *driver, u16 num_bytes)
{
	int b;
	int s;
	int rc;
	unsigned int offset = SENSOR_BLOCK_OFFSET;
	int sensor_type;
	u8 num_sensor_blocks;
	struct sensor_data_block_header *block;
	void *sensors;
	struct device *dev = driver->dev;
	u8 *data = driver->raw_data;
	struct occ_response *resp = &driver->response;

	/* check if the data is valid */
	if (strncmp(&data[SENSOR_STR_OFFSET], "SENSOR", 6) != 0) {
		dev_err(dev, "no SENSOR string in response\n");
		return -ENODATA;
	}

	num_sensor_blocks = data[NUM_SENSOR_BLOCKS_OFFSET];
	if (num_sensor_blocks == 0) {
		dev_warn(dev, "no sensor blocks available\n");
		return -ENODATA;
	}

	memcpy(&resp->header, &data[RESP_HEADER_OFFSET],
	       sizeof(struct occ_poll_header));

	/* data length starts at actual data */
	num_bytes += RESP_HEADER_OFFSET;

	/* translate fields > 1 byte */
	resp->header.error_log_addr_start =
		be32_to_cpu(resp->header.error_log_addr_start);
	resp->header.error_log_length =
		be16_to_cpu(resp->header.error_log_length);

	for (b = 0; b < num_sensor_blocks && b < MAX_OCC_SENSOR_TYPE; b++) {
		if (offset + sizeof(struct sensor_data_block_header) >
		    num_bytes) {
			dev_warn(dev, "exceeded data length\n");
			return 0;
		}

		block = (struct sensor_data_block_header *)&data[offset];
		offset += sizeof(struct sensor_data_block_header);

		if (strncmp(block->sensor_type, "FREQ", 4) == 0)
			sensor_type = FREQ;
		else if (strncmp(block->sensor_type, "TEMP", 4) == 0)
			sensor_type = TEMP;
		else if (strncmp(block->sensor_type, "POWR", 4) == 0)
			sensor_type = POWER;
		else if (strncmp(block->sensor_type, "CAPS", 4) == 0)
			sensor_type = CAPS;
		else {
			dev_warn(dev, "sensor type not supported %.4s\n",
				block->sensor_type);
			continue;
		}

		sensors = &resp->data.blocks[b].sensors;
		if (!sensors) {
			/* first poll response */
			sensors = driver->ops.alloc_sensor(dev, sensor_type,
							   block->num_sensors);
			if (!sensors)
				return -ENOMEM;

			resp->data.blocks[b].sensors = sensors;
			resp->data.sensor_block_id[sensor_type] = b;
			resp->data.blocks[b].header = *block;
		}
		else if (block->sensor_length !=
			 resp->data.blocks[b].header.sensor_length) {
			dev_warn(dev,
				 "different sensor length than first poll\n");
			continue;
		}

		for (s = 0; s < block->num_sensors &&
		     s < resp->data.blocks[b].header.num_sensors; s++) {
			if (offset + block->sensor_length > num_bytes) {
				dev_warn(dev, "exceeded data length\n");
				return 0;
			}

			driver->ops.parse_sensor(data, sensors, sensor_type,
						 offset, s);
			offset += block->sensor_length;
		}
	}

	return 0;
}

static u8 occ_send_cmd(struct occ *driver, u8 seq, u8 type, u16 length,
		       const u8 *data, u8 *resp)
{
	u32 cmd1, cmd2 = 0;
	u16 checksum = 0;
	bool retry = false;
	int i, rc, tries = 0;

	cmd1 = (seq << 24) | (type << 16) | length;
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
		retry = resp[RESP_RETURN_STATUS] == RESP_RETURN_CMD_IN_PRG &&
			tries++ < CMD_IN_PRG_RETRIES;
	} while (retry);

	/* check the occ response */
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

	if (rc < 0)
		dev_warn(driver->dev, "occ bad response:%d\n",
			 resp[RESP_RETURN_STATUS]);

	return rc;

err:
	dev_err(driver->dev, "scom op failed rc:%d\n", rc);
	return rc;
}

static int occ_get_all(struct occ *driver)
{
	int i = 0, rc;
	u8 *occ_data = driver->raw_data;
	u16 num_bytes;
	const u8 poll_cmd_data = OCC_POLL_STAT_SENSOR;
	struct device *dev = driver->dev;

	memset(occ_data, 0, OCC_DATA_MAX);

	rc = occ_send_cmd(driver, 0, OCC_POLL, 1, &poll_cmd_data, occ_data);
	if (rc)
		return rc;

	num_bytes = get_unaligned((u16 *)&occ_data[RESP_DATA_LENGTH]);
	num_bytes = be16_to_cpu(num_bytes);

	if (num_bytes > OCC_DATA_MAX || num_bytes < OCC_DATA_MIN) {
		dev_err(dev, "bad OCC data length:%d\n", num_bytes);
		return -EINVAL;
	}

	/* read remaining data, 8 byte scoms at a time */
	for (i = 8; i < num_bytes + 8; i += 8) {
		rc = driver->bus_ops.getscom(driver->bus, OCB_DATA,
					     (u64 *)&occ_data[i]);
		if (rc) {
			dev_err(dev, "getscom op failed rc:%d\n", rc);
			return rc;
		}
	}

	/* don't need more sanity checks; buffer is alloc'd for max response
	 * size so we just check for valid data in parse_occ_response
	 */
	rc = parse_occ_response(driver, num_bytes);

	return rc;
}

int occ_update_device(struct occ *driver)
{
	int rc = 0;

	mutex_lock(&driver->update_lock);

	if (time_after(jiffies, driver->last_updated + HZ) || !driver->valid) {
		driver->valid = true;

		rc = occ_get_all(driver);
		if (rc)
			driver->valid = false;

		driver->last_updated = jiffies;
	}

	mutex_unlock(&driver->update_lock);

	return rc;
}
EXPORT_SYMBOL(occ_update_device);

void *occ_get_sensor(struct occ *driver, int sensor_type)
{
	int rc;
	int type_id;

	/* occ_update_device locks the update lock */
	rc = occ_update_device(driver);
	if (rc)
		return ERR_PTR(rc);

	type_id = driver->response.data.sensor_block_id[sensor_type];
	if (type_id == -1)
		return ERR_PTR(-ENODATA);

	return driver->response.data.blocks[type_id].sensors;
}
EXPORT_SYMBOL(occ_get_sensor);

int occ_get_sensor_field(struct occ *occ, int sensor_type, int sensor_num,
			 u32 hwmon, long *val)
{
	return occ->ops.get_sensor(occ, sensor_type, sensor_num, hwmon, val);
}
EXPORT_SYMBOL(occ_get_sensor_field);

void occ_get_response_blocks(struct occ *occ, struct occ_blocks **blocks)
{
	*blocks = &occ->response.data;
}
EXPORT_SYMBOL(occ_get_response_blocks);

int occ_set_user_powercap(struct occ *occ, u16 cap)
{
	u8 resp[8];

	cap = cpu_to_be16(cap);

	return occ_send_cmd(occ, 0, OCC_SET_USER_POWR_CAP, 2, (const u8 *)&cap,
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
	driver->raw_data = devm_kzalloc(dev, OCC_DATA_MAX, GFP_KERNEL);
	if (!driver->raw_data)
		return ERR_PTR(-ENOMEM);

	mutex_init(&driver->update_lock);

	return driver;
}
EXPORT_SYMBOL(occ_start);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("OCC hwmon core driver");
MODULE_LICENSE("GPL");
