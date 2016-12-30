/*
 * occ_sysfs.h - OCC sysfs interface
 *
 * This file contains the data structures and function prototypes for the OCC
 * hwmon sysfs entries.
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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __OCC_SYSFS_H__
#define __OCC_SYSFS_H__

#include "occ.h"

struct sensor_group {
	char *name;
	struct sensor_attr_data *sattr;
	struct attribute_group group;
};

struct occ_sysfs_config {
	unsigned int num_caps_fields;
	char **caps_names;
};

struct occ_sysfs {
	struct device *dev;
	struct occ *occ;

	u16 user_powercap;
	bool occ_online;
	struct sensor_group sensor_groups[MAX_OCC_SENSOR_TYPE];
	unsigned long update_interval;
	unsigned int num_caps_fields;
	char **caps_names;
};

struct occ_sysfs *occ_sysfs_start(struct device *dev, struct occ *occ,
				  struct occ_sysfs_config *config);
int occ_sysfs_stop(struct device *dev, struct occ_sysfs *driver);

#endif /* __OCC_SYSFS_H__ */
