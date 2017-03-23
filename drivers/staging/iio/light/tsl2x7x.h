/*
 * Device driver for monitoring ambient light intensity (lux)
 * and proximity (prox) within the TAOS TSL2X7X family of devices.
 *
 * Copyright (c) 2012, TAOS Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA	02110-1301, USA.
 */

#ifndef __TSL2X7X_H
#define __TSL2X7X_H
#include <linux/pm.h>

/**
 * struct tsl2X7X_platform_data - Platform callback, glass and defaults
 * @platform_power:            Suspend/resume platform callback
 * @power_on:                  Power on callback
 * @power_off:                 Power off callback
 * @platform_lux_table:        Device specific glass coefficents
 * @platform_default_settings: Device specific power on defaults
 *
 */
struct tsl2X7X_platform_data {
	int (*platform_power)(struct device *dev, pm_message_t);
	int (*power_on)(struct iio_dev *indio_dev);
	int (*power_off)(struct i2c_client *dev);
	struct tsl2x7x_lux platform_lux_table[TSL2X7X_MAX_LUX_TABLE_SIZE];
	struct tsl2x7x_settings *platform_default_settings;
};

#endif /* __TSL2X7X_H */
