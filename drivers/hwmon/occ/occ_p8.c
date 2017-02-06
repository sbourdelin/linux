/*
 * occ_p8.c - OCC hwmon driver
 *
 * This file contains the Power8-specific methods and data structures for
 * the OCC hwmon driver.
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
#include <linux/device.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "occ.h"
#include "occ_p8.h"

/* P8 OCC sensor data format */
struct p8_occ_sensor {
	u16 sensor_id;
	u16 value;
};

struct p8_power_sensor {
	u16 sensor_id;
	u32 update_tag;
	u32 accumulator;
	u16 value;
};

struct p8_caps_sensor {
	u16 curr_powercap;
	u16 curr_powerreading;
	u16 norm_powercap;
	u16 max_powercap;
	u16 min_powercap;
	u16 user_powerlimit;
};

static const u32 p8_sensor_hwmon_configs[MAX_OCC_SENSOR_TYPE] = {
	HWMON_I_INPUT | HWMON_I_LABEL,	/* freq: value | label */
	HWMON_T_INPUT | HWMON_T_LABEL,	/* temp: value | label */
	/* power: value | label | accumulator | update_tag */
	HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_AVERAGE |
		HWMON_P_AVERAGE_INTERVAL,
	/* caps: curr | max | min | norm | user */
	HWMON_P_CAP | HWMON_P_CAP_MAX | HWMON_P_CAP_MIN | HWMON_P_MAX |
		HWMON_P_ALARM,
};

void p8_parse_sensor(u8 *data, void *sensor, int sensor_type, int off,
		     int snum)
{
	switch (sensor_type) {
	case FREQ:
	case TEMP:
	{
		struct p8_occ_sensor *os =
			&(((struct p8_occ_sensor *)sensor)[snum]);

		os->sensor_id = be16_to_cpu(get_unaligned((u16 *)&data[off]));
		os->value = be16_to_cpu(get_unaligned((u16 *)&data[off + 2]));
	}
		break;
	case POWER:
	{
		struct p8_power_sensor *ps =
			&(((struct p8_power_sensor *)sensor)[snum]);

		ps->sensor_id = be16_to_cpu(get_unaligned((u16 *)&data[off]));
		ps->update_tag =
			be32_to_cpu(get_unaligned((u32 *)&data[off + 2]));
		ps->accumulator =
			be32_to_cpu(get_unaligned((u32 *)&data[off + 6]));
		ps->value = be16_to_cpu(get_unaligned((u16 *)&data[off + 10]));
	}
		break;
	case CAPS:
	{
		struct p8_caps_sensor *cs =
			&(((struct p8_caps_sensor *)sensor)[snum]);

		cs->curr_powercap =
			be16_to_cpu(get_unaligned((u16 *)&data[off]));
		cs->curr_powerreading =
			be16_to_cpu(get_unaligned((u16 *)&data[off + 2]));
		cs->norm_powercap =
			be16_to_cpu(get_unaligned((u16 *)&data[off + 4]));
		cs->max_powercap =
			be16_to_cpu(get_unaligned((u16 *)&data[off + 6]));
		cs->min_powercap =
			be16_to_cpu(get_unaligned((u16 *)&data[off + 8]));
		cs->user_powerlimit =
			be16_to_cpu(get_unaligned((u16 *)&data[off + 10]));
	}
		break;
	};
}

void *p8_alloc_sensor(struct device *dev, int sensor_type, int num_sensors)
{
	switch (sensor_type) {
	case FREQ:
	case TEMP:
		return devm_kzalloc(dev, num_sensors *
				    sizeof(struct p8_occ_sensor), GFP_KERNEL);
	case POWER:
		return devm_kzalloc(dev, num_sensors *
				    sizeof(struct p8_power_sensor),
				    GFP_KERNEL);
	case CAPS:
		return devm_kzalloc(dev, num_sensors *
				    sizeof(struct p8_caps_sensor), GFP_KERNEL);
	default:
		return NULL;
	}
}

int p8_get_sensor(struct occ *driver, int sensor_type, int sensor_num,
		  u32 hwmon, long *val)
{
	int rc = 0;
	void *sensor;

	if (sensor_type == POWER) {
		if (hwmon == hwmon_power_cap || hwmon == hwmon_power_cap_max ||
		    hwmon == hwmon_power_cap_min || hwmon == hwmon_power_max ||
		    hwmon == hwmon_power_alarm)
			sensor_type = CAPS;
	}

	sensor = occ_get_sensor(driver, sensor_type);
	if (!sensor)
		return -ENODEV;

	switch (sensor_type) {
	case FREQ:
	case TEMP:
	{
		struct p8_occ_sensor *os =
			&(((struct p8_occ_sensor *)sensor)[sensor_num]);

		if (hwmon == hwmon_in_input || hwmon == hwmon_temp_input)
			*val = os->value;
		else if (hwmon == hwmon_in_label || hwmon == hwmon_temp_label)
			*val = os->sensor_id;
		else
			rc = -EOPNOTSUPP;
	}
		break;
	case POWER:
	{
		struct p8_power_sensor *ps =
			&(((struct p8_power_sensor *)sensor)[sensor_num]);

		switch (hwmon) {
		case hwmon_power_input:
			*val = ps->value;
			break;
		case hwmon_power_label:
			*val = ps->sensor_id;
			break;
		case hwmon_power_average:
			*val = ps->accumulator;
			break;
		case hwmon_power_average_interval:
			*val = ps->update_tag;
			break;
		default:
			rc = -EOPNOTSUPP;
		}
	}
		break;
	case CAPS:
	{
		struct p8_caps_sensor *cs =
			&(((struct p8_caps_sensor *)sensor)[sensor_num]);

		switch (hwmon) {
		case hwmon_power_cap:
			*val = cs->curr_powercap;
			break;
		case hwmon_power_cap_max:
			*val = cs->max_powercap;
			break;
		case hwmon_power_cap_min:
			*val = cs->min_powercap;
			break;
		case hwmon_power_max:
			*val = cs->norm_powercap;
			break;
		case hwmon_power_alarm:
			*val = cs->user_powerlimit;
			break;
		default:
			rc = -EOPNOTSUPP;
		}
	}
		break;
	default:
		rc = -EINVAL;
	}

	return rc;
}

static const struct occ_ops p8_ops = {
	.parse_sensor = p8_parse_sensor,
	.alloc_sensor = p8_alloc_sensor,
	.get_sensor = p8_get_sensor,
};

static const struct occ_config p8_config = {
	.command_addr = 0xFFFF6000,
	.response_addr = 0xFFFF7000,
};

const u32 *p8_get_sensor_hwmon_configs()
{
	return p8_sensor_hwmon_configs;
}
EXPORT_SYMBOL(p8_get_sensor_hwmon_configs);

struct occ *p8_occ_start(struct device *dev, void *bus,
			 struct occ_bus_ops *bus_ops)
{
	return occ_start(dev, bus, bus_ops, &p8_ops, &p8_config);
}
EXPORT_SYMBOL(p8_occ_start);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("P8 OCC sensors");
MODULE_LICENSE("GPL");
