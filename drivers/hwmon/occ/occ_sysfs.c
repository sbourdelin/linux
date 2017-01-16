/*
 * occ_sysfs.c - OCC sysfs interface
 *
 * This file contains the methods and data structures for implementing the OCC
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

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "occ.h"
#include "occ_sysfs.h"

#define RESP_RETURN_CMD_INVAL	0x13

static int occ_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			  u32 attr, int channel, long *val)
{
	int rc = 0;
	struct occ_sysfs *driver = dev_get_drvdata(dev);
	struct occ *occ = driver->occ;

	switch (type) {
	case hwmon_in:
		rc = occ_get_sensor_value(occ, FREQ, channel, attr, val);
		break;
	case hwmon_temp:
		rc = occ_get_sensor_value(occ, TEMP, channel, attr, val);
		break;
	case hwmon_power:
		rc = occ_get_sensor_value(occ, POWER, channel, attr, val);
		break;
	default:
		rc = -EOPNOTSUPP;
	}

	return rc;
}

static int occ_hwmon_read_string(struct device *dev,
				 enum hwmon_sensor_types type, u32 attr,
				 int channel, char **str)
{
	int rc;
	unsigned long val = 0;

	if (!((type == hwmon_in && attr == hwmon_in_label) ||
	    (type == hwmon_temp && attr == hwmon_temp_label) ||
	    (type == hwmon_power && attr == hwmon_power_label)))
		return -EOPNOTSUPP;

	rc = occ_hwmon_read(dev, type, attr, channel, &val);
	if (rc < 0)
		return rc;

	rc = snprintf(*str, PAGE_SIZE - 1, "%ld", val);
	if (rc > 0)
		rc = 0;

	return rc;
}

static int occ_hwmon_write(struct device *dev, enum hwmon_sensor_types type,
			   u32 attr, int channel, long val)
{
	int rc = 0;
	struct occ_sysfs *driver = dev_get_drvdata(dev);

	if (type == hwmon_chip && attr == hwmon_chip_update_interval) {
		occ_set_update_interval(driver->occ, val);
		return 0;
	} else if (type == hwmon_power && attr == hwmon_power_alarm) {
		rc = occ_set_user_powercap(driver->occ, val);
		if (rc) {
			if (rc == RESP_RETURN_CMD_INVAL) {
				dev_err(dev,
					"set invalid powercap value: %ld\n",
					val);
				return -EINVAL;
			}

			dev_err(dev, "set user powercap failed: 0x:%x\n", rc);
			return rc;
		}

		driver->user_powercap = val;

		return rc;
	}

	return -EOPNOTSUPP;
}

static umode_t occ_is_visible(const void *data, enum hwmon_sensor_types type,
			      u32 attr, int channel)
{
	const struct occ_sysfs *driver = data;

	switch (type) {
	case hwmon_chip:
		if (attr == hwmon_chip_update_interval)
			return S_IRUGO | S_IWUSR;
		break;
	case hwmon_in:
		if (BIT(attr) & driver->sensor_hwmon_configs[0])
			return S_IRUGO;
		break;
	case hwmon_temp:
		if (BIT(attr) & driver->sensor_hwmon_configs[1])
			return S_IRUGO;
		break;
	case hwmon_power:
		/* user power limit */
		if (attr == hwmon_power_alarm)
			return S_IRUGO | S_IWUSR;
		else if ((BIT(attr) & driver->sensor_hwmon_configs[2]) ||
			 (BIT(attr) & driver->sensor_hwmon_configs[3]))
			return S_IRUGO;
		break;
	default:
		return 0;
	}

	return 0;
}

static const struct hwmon_ops occ_hwmon_ops = {
	.is_visible = occ_is_visible,
	.read = occ_hwmon_read,
	.read_string = occ_hwmon_read_string,
	.write = occ_hwmon_write,
};

static const u32 occ_chip_config[] = {
	HWMON_C_UPDATE_INTERVAL,
	0
};

static const struct hwmon_channel_info occ_chip = {
	.type = hwmon_chip,
	.config = occ_chip_config
};

static const enum hwmon_sensor_types occ_sensor_types[MAX_OCC_SENSOR_TYPE] = {
	hwmon_in,
	hwmon_temp,
	hwmon_power,
	hwmon_power
};

struct occ_sysfs *occ_sysfs_start(struct device *dev, struct occ *occ,
				  const u32 *sensor_hwmon_configs,
				  const char *name)
{
	bool master_occ = false;
	int rc, i, j, sensor_num, index = 0, id;
	char *brk;
	struct occ_blocks *resp = NULL;
	u32 *sensor_config;
	struct occ_sysfs *hwmon = devm_kzalloc(dev, sizeof(struct occ_sysfs),
					       GFP_KERNEL);
	if (!hwmon)
		return ERR_PTR(-ENOMEM);

	/* need space for null-termination and occ chip */
	hwmon->occ_sensors =
		devm_kzalloc(dev, sizeof(struct hwmon_channel_info *) *
			     (MAX_OCC_SENSOR_TYPE + 2), GFP_KERNEL);
	if (!hwmon->occ_sensors)
		return ERR_PTR(-ENOMEM);

	hwmon->occ = occ;
	hwmon->sensor_hwmon_configs = (u32 *)sensor_hwmon_configs;
	hwmon->occ_info.ops = &occ_hwmon_ops;
	hwmon->occ_info.info =
		(const struct hwmon_channel_info **)hwmon->occ_sensors;

	occ_get_response_blocks(occ, &resp);

	for (i = 0; i < MAX_OCC_SENSOR_TYPE; ++i)
		resp->sensor_block_id[i] = -1;

	/* read sensor data from occ */
	rc = occ_update_device(occ);
	if (rc) {
		dev_err(dev, "cannot get occ sensor data: %d\n", rc);
		return ERR_PTR(rc);
	}
	if (!resp->blocks)
		return ERR_PTR(-ENOMEM);

	master_occ = resp->sensor_block_id[CAPS] >= 0;

	for (i = 0; i < MAX_OCC_SENSOR_TYPE; i++) {
		id = resp->sensor_block_id[i];
		if (id < 0)
			continue;

		sensor_num = resp->blocks[id].header.sensor_num;
		/* need null-termination */
		sensor_config = devm_kzalloc(dev,
					     sizeof(u32) * (sensor_num + 1),
					     GFP_KERNEL);
		if (!sensor_config)
			return ERR_PTR(-ENOMEM);

		for (j = 0; j < sensor_num; j++)
			sensor_config[j] = sensor_hwmon_configs[i];

		hwmon->occ_sensors[index] =
			devm_kzalloc(dev, sizeof(struct hwmon_channel_info),
				     GFP_KERNEL);
		if (!hwmon->occ_sensors[index])
			return ERR_PTR(-ENOMEM);

		hwmon->occ_sensors[index]->type = occ_sensor_types[i];
		hwmon->occ_sensors[index]->config = sensor_config;
		index++;
	}

	/* only need one of these for any number of occs */
	if (master_occ)
		hwmon->occ_sensors[index] =
			(struct hwmon_channel_info *)&occ_chip;

	/* search for bad chars */
	strncpy(hwmon->hwmon_name, name, OCC_HWMON_NAME_LENGTH);
	brk = strpbrk(hwmon->hwmon_name, "-* \t\n");
	while (brk) {
		*brk = '_';
		brk = strpbrk(brk,  "-* \t\n");
	}

	hwmon->dev = devm_hwmon_device_register_with_info(dev,
							  hwmon->hwmon_name,
							  hwmon,
							  &hwmon->occ_info,
							  NULL);
	if (IS_ERR(hwmon->dev)) {
		dev_err(dev, "cannot register hwmon device %s: %ld\n",
			hwmon->hwmon_name, PTR_ERR(hwmon->dev));
		return ERR_CAST(hwmon->dev);
	}

	return hwmon;
}
EXPORT_SYMBOL(occ_sysfs_start);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("OCC sysfs driver");
MODULE_LICENSE("GPL");
