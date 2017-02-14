/*
 * occ_p9.h - OCC hwmon driver
 *
 * This file contains Power9-specific function prototypes
 *
 * Copyright 2017 IBM Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __OCC_P9_H__
#define __OCC_P9_H__

struct device;
struct occ;
struct occ_bus_ops;

const u32 *p9_get_sensor_hwmon_configs(void);
struct occ *p9_occ_init(struct device *dev, void *bus,
			const struct occ_bus_ops *bus_ops);

#endif /* __OCC_P9_H__ */
