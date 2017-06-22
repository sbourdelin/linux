/*
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef OCC_COMMON_H
#define OCC_COMMON_H

#include <linux/hwmon-sysfs.h>
#include <linux/sysfs.h>

#define OCC_ERROR_COUNT_THRESHOLD	2

#define OCC_NUM_STATUS_ATTRS		8

#define OCC_RESP_DATA_BYTES		4089

#define OCC_SAFE_TIMEOUT		msecs_to_jiffies(60000) /* 1 min */
#define OCC_UPDATE_FREQUENCY		msecs_to_jiffies(1000)
#define OCC_TIMEOUT_MS			5000
#define OCC_CMD_IN_PRG_MS		100

/* OCC return codes */
#define RESP_RETURN_CMD_IN_PRG		0xFF
#define RESP_RETURN_SUCCESS		0
#define RESP_RETURN_CMD_INVAL		0x11
#define RESP_RETURN_CMD_LEN		0x12
#define RESP_RETURN_DATA_INVAL		0x13
#define RESP_RETURN_CHKSUM		0x14
#define RESP_RETURN_OCC_ERR		0x15
#define RESP_RETURN_STATE		0x16

/* OCC status bits */
#define OCC_STAT_MASTER			0x80
#define OCC_STAT_ACTIVE			0x01
#define OCC_EXT_STAT_DVFS_OT		0x80
#define OCC_EXT_STAT_DVFS_POWER		0x40
#define OCC_EXT_STAT_MEM_THROTTLE	0x20
#define OCC_EXT_STAT_QUICK_DROP		0x10

/* OCC state enumeration */
#define OCC_STATE_SAFE			4

/* Same response format for all OCC versions.
 * Allocate the largest possible response.
 */
struct occ_response {
	u8 seq_no;
	u8 cmd_type;
	u8 return_status;
	u16 data_length_be;
	u8 data[OCC_RESP_DATA_BYTES];
	u16 checksum_be;
} __packed;

struct occ_sensor_data_block_header {
	u8 eye_catcher[4];
	u8 reserved;
	u8 sensor_format;
	u8 sensor_length;
	u8 num_sensors;
} __packed;

struct occ_sensor_data_block {
	struct occ_sensor_data_block_header header;
	u32 data;
} __packed;

struct occ_poll_response_header {
	u8 status;
	u8 ext_status;
	u8 occs_present;
	u8 config_data;
	u8 occ_state;
	u8 mode;
	u8 ips_status;
	u8 error_log_id;
	u32 error_log_start_address_be;
	u16 error_log_length_be;
	u16 reserved;
	u8 occ_code_level[16];
	u8 eye_catcher[6];
	u8 num_sensor_data_blocks;
	u8 sensor_data_block_header_version;
} __packed;

struct occ_poll_response {
	struct occ_poll_response_header header;
	struct occ_sensor_data_block block;
} __packed;

struct occ_sensor {
	u8 num_sensors;
	u8 version;
	void *data;	/* pointer to sensor data start within response */
};

/* OCC only provides one sensor data block of each type, but any number of
 * sensors within that block.
 */
struct occ_sensors {
	struct occ_sensor temp;
	struct occ_sensor freq;
	struct occ_sensor power;
	struct occ_sensor caps;
	struct occ_sensor extended;
};

/* Use our own attribute struct so we can dynamically allocate space for the
 * name.
 */
struct occ_attribute {
	char name[32];
	struct sensor_device_attribute_2 sensor;
};

struct occ {
	struct device *bus_dev;

	struct occ_response resp;
	struct occ_sensors sensors;

	u8 poll_cmd_data;		/* to perform OCC poll command */
	int (*send_cmd)(struct occ *occ, u8 *cmd);

	unsigned long last_update;
	struct mutex lock;

	struct device *hwmon;
	unsigned int num_attrs;
	struct occ_attribute *attrs;
	struct attribute_group group;
	const struct attribute_group *groups[2];

	/* non-hwmon attributes for more OCC properties */
	struct sensor_device_attribute *status_attrs;

	int error;
	unsigned int error_count;		/* num errors observed */
	unsigned int bad_present_count;		/* num polls w/bad num occs */
	unsigned long last_safe;		/* time entered safe state */
};

int occ_setup(struct occ *occ, const char *name);
int occ_shutdown(struct occ *occ);

#endif /* OCC_COMMON_H */
