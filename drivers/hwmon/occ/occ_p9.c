/*
 * occ_p9.c - OCC hwmon driver
 *
 * This file contains the Power9-specific methods and data structures for
 * the OCC hwmon driver.
 *
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
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

/* value, sensor_id */
#define FREQ_SENSOR_CONFIG	(HWMON_I_INPUT | HWMON_I_LABEL)

/* value, sensor_id, fru_type */
#define TEMP_SENSOR_CONFIG	(HWMON_T_INPUT | HWMON_T_LABEL | HWMON_T_TYPE)

/* value, sensor_id, accumulator[0], accumulator[1], update_tag,
 * (function_id | (appss_channel << 8)
 */
#define POWER_SENSOR_CONFIG						\
(									\
	HWMON_P_INPUT | HWMON_P_LABEL | HWMON_P_AVERAGE_MIN |		\
	HWMON_P_AVERAGE_MAX | HWMON_P_AVERAGE_INTERVAL |		\
	HWMON_P_RESET_HISTORY						\
)

/* curr_powercap, max_powercap, min_powercap, norm_powercap, user_powerlimit
 * user_powerlimit_source
 */
#define CAPS_SENSOR_CONFIG						\
(									\
	HWMON_P_CAP | HWMON_P_CAP_MAX | HWMON_P_CAP_MIN | HWMON_P_MAX |	\
	HWMON_P_ALARM | HWMON_P_CAP_ALARM				\
)

static const u32 p9_sensor_hwmon_configs[MAX_OCC_SENSOR_TYPE] = {
	FREQ_SENSOR_CONFIG,
	TEMP_SENSOR_CONFIG,
	POWER_SENSOR_CONFIG,
	CAPS_SENSOR_CONFIG,
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
		ts->fru_type = data[off + 4];
		ts->value = data[off + 5];
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

void *p9_alloc_sensor(struct device *dev, int sensor_type, int num_sensors)
{
	switch (sensor_type) {
	case FREQ:
		return devm_kzalloc(dev, num_sensors *
				    sizeof(struct p9_freq_sensor), GFP_KERNEL);
	case TEMP:
		return devm_kzalloc(dev, num_sensors *
				    sizeof(struct p9_temp_sensor), GFP_KERNEL);
	case POWER:
		return devm_kzalloc(dev, num_sensors *
				    sizeof(struct p9_power_sensor),
				    GFP_KERNEL);
	case CAPS:
		return devm_kzalloc(dev, num_sensors *
				    sizeof(struct p9_caps_sensor), GFP_KERNEL);
	default:
		return NULL;
	}
}

int p9_get_sensor(struct occ *driver, int sensor_type, int sensor_num,
		  u32 hwmon, long *val)
{
	int rc = 0;
	void *sensor;

	if (sensor_type == POWER) {
		if (hwmon == hwmon_power_cap || hwmon == hwmon_power_cap_max ||
		    hwmon == hwmon_power_cap_min || hwmon == hwmon_power_max ||
		    hwmon == hwmon_power_alarm ||
		    hwmon == hwmon_power_cap_alarm)
			sensor_type = CAPS;
	}

	sensor = occ_get_sensor(driver, sensor_type);
	if (!sensor)
		return -ENODEV;

	switch (sensor_type) {
	case FREQ:
	{
		struct p9_freq_sensor *fs =
			&(((struct p9_freq_sensor *)sensor)[sensor_num]);

		switch (hwmon) {
		case hwmon_in_input:
			*val = fs->value;
			break;
		case hwmon_in_label:
			*val = fs->sensor_id;
			break;
		default:
			rc = -EOPNOTSUPP;
		}
	}
		break;
	case TEMP:
	{
		struct p9_temp_sensor *ts =
			&(((struct p9_temp_sensor *)sensor)[sensor_num]);

		switch (hwmon) {
		case hwmon_temp_input:
			*val = ts->value;
			break;
		case hwmon_temp_type:
			*val = ts->fru_type;
			break;
		case hwmon_temp_label:
			*val = ts->sensor_id;
			break;
		default:
			rc = -EOPNOTSUPP;
		}
	}
		break;
	case POWER:
	{
		struct p9_power_sensor *ps =
			&(((struct p9_power_sensor *)sensor)[sensor_num]);

		switch (hwmon) {
		case hwmon_power_input:
			*val = ps->value;
			break;
		case hwmon_power_label:
			*val = ps->sensor_id;
			break;
		case hwmon_power_average_min:
			*val = ((u32 *)(&ps->accumulator))[0];
			break;
		case hwmon_power_average_max:
			*val = ((u32 *)(&ps->accumulator))[1];
			break;
		case hwmon_power_average_interval:
			*val = ps->update_tag;
			break;
		case hwmon_power_reset_history:
			*val = ps->function_id | (ps->apss_channel << 8);
			break;
		default:
			rc = -EOPNOTSUPP;
		}
	}
		break;
	case CAPS:
	{
		struct p9_caps_sensor *cs =
			&(((struct p9_caps_sensor *)sensor)[sensor_num]);

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
		case hwmon_power_cap_alarm:
			*val = cs->user_powerlimit_source;
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

static const struct occ_ops p9_ops = {
	.parse_sensor = p9_parse_sensor,
	.alloc_sensor = p9_alloc_sensor,
	.get_sensor = p9_get_sensor,
};

static const struct occ_init_data p9_init = {
	.command_addr = 0xFFFBE000,
	.response_addr = 0xFFFBF000,
	.ops = &p9_ops,
};

const u32 *p9_get_sensor_hwmon_configs()
{
	return p9_sensor_hwmon_configs;
}

struct occ *p9_occ_init(struct device *dev, void *bus,
			const struct occ_bus_ops *bus_ops)
{
	return occ_init(dev, bus, bus_ops, &p9_init);
}

MODULE_AUTHOR("Eddie James <eajames@us.ibm.com>");
MODULE_DESCRIPTION("P9 OCC sensors");
MODULE_LICENSE("GPL");
