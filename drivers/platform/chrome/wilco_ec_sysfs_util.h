/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wilco_ec_sysfs_util - helpers for sysfs attributes of Wilco EC
 *
 * Copyright 2018 Google LLC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef WILCO_EC_SYSFS_UTIL_H
#define WILCO_EC_SYSFS_UTIL_H

#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/string.h>

/**
 * device_from_kobject() - Get EC device from subdirectory's kobject.
 * @kobj: kobject associated with a subdirectory
 *
 * When we place attributes within directories within the sysfs filesystem,
 * at each callback we get a reference to the kobject representing the directory
 * that that attribute is in. Somehow we need to get a pointer to the EC device.
 * This goes up the directory structure a number of levels until reaching the
 * top level for the EC device, and then finds the device from the root kobject.
 *
 * Example: for attribute GOOG000C:00/properties/peakshift/sunday,
 * we would go up two levels, from peakshift to properties and then from
 * properties to GOOG000C:00
 *
 * Return: a pointer to the device struct representing the EC.
 */
static inline struct device *device_from_kobject(struct kobject *kobj)
{
	while (strcmp(kobj->name, "GOOG000C:00") != 0)
		kobj = kobj->parent;
	return container_of(kobj, struct device, kobj);
}

#endif
