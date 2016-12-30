/*
 * p9.c - OCC hwmon driver
 *
 * This file contains the Power9-specific methods and data structures for
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

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <asm/unaligned.h>

#include "occ.h"
#include "occ_p9.h"

/* P9 OCC sensor data format */
struct p9_temp_sensor {
	u32 sensor_id;
	u8 fru_type;
	u8 value;
};

struct p9_freq_sensor {
	u32 sensor_id;
	u16 value;
};

struct p9_power_sensor {
	u32 sensor_id;
	u8 function_id;
	u8 apss_channel;
	u16 reserved;
	u32 update_tag;
	u64 accumulator;
	u16 value;
};

struct p9_caps_sensor {
	u16 curr_powercap;
	u16 curr_powerreading;
	u16 norm_powercap;
	u16 max_powercap;
	u16 min_powercap;
	u16 user_powerlimit;
	u8 user_powerlimit_source;
};

void p9_parse_sensor(u8 *data, void *sensor, int sensor_type, int off,
		     int snum)
{
	switch (sensor_type) {
	case FREQ:
	{
		struct p9_freq_sensor *fs =
			&(((struct p9_freq_sensor *)sensor)[snum]);

		fs->sensor_id = be32_to_cpu(get_unaligned((u32 *)&data[off]));
		fs->value = be16_to_cpu(get_unaligned((u16 *)&data[off + 4]));
	}
		break;
	case TEMP:
	{
		struct p9_temp_sensor *ts =
			&(((struct p9_temp_sensor *)sensor)[snum]);

		ts->sensor_id = be32_to_cpu(get_unaligned((u32 *)&data[off]));
		fs->fru_type = data[off + 4];
		fs->value = data[off + 5];
	}
		break;
	case POWER:
	{
		struct p9_power_sensor *ps =
			&(((struct p9_power_sensor *)sensor)[snum]);

		ps->sensor_id = be32_to_cpu(get_unaligned((u32 *)&data[off]));
		ps->function_id = data[off + 4];
		ps->apss_channel = data[off + 5];
		ps->update_tag =
			be32_to_cpu(get_unaligned((u32 *)&data[off + 8]));
		ps->accumulator =
			be64_to_cpu(get_unaligned((u64 *)&data[off + 12]));
		ps->value = be16_to_cpu(get_unaligned((u16 *)&data[off + 20]));
	}
		break;
	case CAPS:
	{
		struct p9_caps_sensor *cs =
			&(((struct p9_caps_sensor *)sensor)[snum]);

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
		cs->user_powerlimit_source = data[off + 12];
	}
		break;
	};
}

void *p9_alloc_sensor(int sensor_type, int num_sensors)
{
	switch (sensor_type) {
	case FREQ:
		return kcalloc(num_sensors, sizeof(struct p9_freq_sensor),
			       GFP_KERNEL);
	case TEMP:
		return kcalloc(num_sensors, sizeof(struct p9_temp_sensor),
			       GFP_KERNEL);
	case POWER:
		return kcalloc(num_sensors, sizeof(struct p9_power_sensor),
			       GFP_KERNEL);
	case CAPS:
		return kcalloc(num_sensors, sizeof(struct p9_caps_sensor),
			       GFP_KERNEL);
	default:
		return NULL;
	}
}

int p9_get_sensor_value(struct occ *driver, int sensor_type, int snum)
{
	void *sensor;

	if (sensor_type == CAPS)
		return -EINVAL;

	sensor = occ_get_sensor(driver, sensor_type);
	if (!sensor)
		return -ENODEV;

	switch (sensor_type) {
	case FREQ:
		return ((struct p9_freq_sensor *)sensor)[snum].value;
	case TEMP:
		return ((struct p9_temp_sensor *)sensor)[snum].value;
	case POWER:
		return ((struct p9_power_sensor *)sensor)[snum].value;
	default:
		return -EINVAL;
	}
}

int p9_get_sensor_id(struct occ *driver, int sensor_type, int snum)
{
	void *sensor;

	if (sensor_type == CAPS)
		return -EINVAL;

	sensor = occ_get_sensor(driver, sensor_type);
	if (!sensor)
		return -ENODEV;

	switch (sensor_type) {
	case FREQ:
		return ((struct p9_freq_sensor *)sensor)[snum].sensor_id;
	case TEMP:
		return ((struct p9_temp_sensor *)sensor)[snum].sensor_id;
	case POWER:
		return ((struct p9_power_sensor *)sensor)[snum].sensor_id;
	default:
		return -EINVAL;
	}
}

int p9_get_caps_value(void *sensor, int snum, int caps_field)
{
	struct p9_caps_sensor *caps_sensor = sensor;

	switch (caps_field) {
	case 0:
		return caps_sensor[snum].curr_powercap;
	case 1:
		return caps_sensor[snum].curr_powerreading;
	case 2:
		return caps_sensor[snum].norm_powercap;
	case 3:
		return caps_sensor[snum].max_powercap;
	case 4:
		return caps_sensor[snum].min_powercap;
	case 5:
		return caps_sensor[snum].user_powerlimit;
	case 6:
		return caps_sensor[snum].user_powerlimit_source;
	default:
		return -EINVAL;
	}
}
static const struct occ_ops p9_ops = {
	.parse_sensor = p9_parse_sensor,
	.alloc_sensor = p9_alloc_sensor,
	.get_sensor_value = p9_get_sensor_value,
	.get_sensor_id = p9_get_sensor_id,
	.get_caps_value = p9_get_caps_value,
};

static const struct occ_config p9_config = {
	.command_addr = 0xFFFBE000,
	.response_addr = 0xFFFBF000,
};

struct occ *p9_occ_start(struct device *dev, void *bus,
			 struct occ_bus_ops *bus_ops)
{
	return occ_start(dev, bus, bus_ops, &p9_ops, &p9_config);
}
EXPORT_SYMBOL(occ_p9_start);

int p9_occ_stop(struct occ *occ)
{
	return occ_stop(occ);
}
EXPORT_SYMBOL(occ_p9_stop);

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("P9 OCC sensors");
MODULE_LICENSE("GPL");
