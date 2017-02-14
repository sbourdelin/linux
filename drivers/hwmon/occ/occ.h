/*
 * occ.h - hwmon OCC driver
 *
 * This file contains data structures and function prototypes for common access
 * between different bus protocols and host systems.
 *
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __OCC_H__
#define __OCC_H__

#include "scom.h"

struct device;
struct occ;

enum sensor_type {
	FREQ = 0,
	TEMP,
	POWER,
	CAPS,
	MAX_OCC_SENSOR_TYPE
};

/* sensor_data_block_header
 * structure to match the raw occ sensor block header
 */
struct sensor_data_block_header {
	u8 sensor_type[4];
	u8 reserved0;
	u8 sensor_format;
	u8 sensor_length;
	u8 num_sensors;
} __attribute__((packed, aligned(4)));

struct sensor_data_block {
	struct sensor_data_block_header header;
	void *sensors;
};

struct occ_blocks {
	int sensor_block_id[MAX_OCC_SENSOR_TYPE];
	struct sensor_data_block blocks[MAX_OCC_SENSOR_TYPE];
};

struct occ_ops {
	void (*parse_sensor)(u8 *data, void *sensor, int sensor_type, int off,
			     int snum);
	void *(*alloc_sensor)(struct device *dev, int sensor_type,
			      int num_sensors);
	int (*get_sensor)(struct occ *driver, int sensor_type, int sensor_num,
			  u32 hwmon, long *val);
};

struct occ_init_data {
	u32 command_addr;
	u32 response_addr;
	const struct occ_ops *ops;
};

struct occ *occ_init(struct device *dev, void *bus,
		     const struct occ_bus_ops *bus_ops,
		     const struct occ_init_data *init);
void *occ_get_sensor(struct occ *occ, int sensor_type);
int occ_get_sensor_field(struct occ *occ, int sensor_type, int sensor_num,
			 u32 hwmon, long *val);
void occ_get_response_blocks(struct occ *occ, struct occ_blocks **blocks);
int occ_update_device(struct occ *driver);
int occ_set_user_powercap(struct occ *occ, u16 cap);

#endif /* __OCC_H__ */
