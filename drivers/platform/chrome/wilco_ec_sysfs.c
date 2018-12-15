// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_sysfs - Sysfs attributes for Wilco Embedded Controller
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

#include <linux/ctype.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include "wilco_ec.h"
#include "wilco_ec_legacy.h"

#define WILCO_EC_ATTR_RO(_name)						\
__ATTR(_name, 0444, wilco_ec_##_name##_show, NULL)

#define WILCO_EC_ATTR_WO(_name)						\
__ATTR(_name, 0200, NULL, wilco_ec_##_name##_store)

#define WILCO_EC_ATTR_RW(_name)						\
__ATTR(_name, 0644, wilco_ec_##_name##_show, wilco_ec_##_name##_store)

/* Make top-level attributes, which will live inside GOOG000C:00/ */

static struct device_attribute version_attr = WILCO_EC_ATTR_RO(version);
static struct device_attribute stealth_attr = WILCO_EC_ATTR_WO(stealth_mode);
#ifdef CONFIG_WILCO_EC_SYSFS_RAW
static struct device_attribute raw_attr = WILCO_EC_ATTR_RW(raw);
#endif

static struct attribute *wilco_ec_toplevel_attrs[] = {
	&version_attr.attr,
	&stealth_attr.attr,
#ifdef CONFIG_WILCO_EC_SYSFS_RAW
	&raw_attr.attr,
#endif
	NULL
};

ATTRIBUTE_GROUPS(wilco_ec_toplevel);

/**
 * wilco_ec_sysfs_init() - Initialize the sysfs directories and attributes
 * @dev: The device representing the EC
 *
 * Creates the sysfs directory structure and populates it with all attributes.
 * If there is a problem it will clean up the entire filesystem.
 *
 * Return 0 on success, -ENOMEM on failure creating directories or attibutes.
 */
int wilco_ec_sysfs_init(struct wilco_ec_device *ec)
{
	struct device *dev = ec->dev;
	int ret;

	// add the top-level attributes
	ret = sysfs_create_groups(&dev->kobj, wilco_ec_toplevel_groups);
	if (ret) {
		dev_err(dev, "failed to create sysfs filesystem!");
		return -ENOMEM;
	}

	return 0;
}

void wilco_ec_sysfs_remove(struct wilco_ec_device *ec)
{
	struct device *dev = ec->dev;

	/* go upwards through the directory structure */
	sysfs_remove_groups(&dev->kobj, wilco_ec_toplevel_groups);
}
