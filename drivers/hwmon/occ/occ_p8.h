/*
 * occ_p8.h - OCC hwmon driver
 *
 * This file contains Power8-specific function prototypes
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

#ifndef __OCC_P8_H__
#define __OCC_P8_H__

#include "scom.h"

struct device;

struct occ *p8_occ_start(struct device *dev, void *bus,
			 struct occ_bus_ops *bus_ops);
int p8_occ_stop(struct occ *occ);

#endif /* __OCC_P8_H__ */
