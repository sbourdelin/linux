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

struct occ;
struct device;
struct occ_sysfs;

struct occ_sysfs *occ_sysfs_start(struct device *dev, struct occ *occ,
				  const u32 *sensor_hwmon_configs,
				  const char *name);
#endif /* __OCC_SYSFS_H__ */
