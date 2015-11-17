/*
 * pv88060.h - Regulator device driver for PV88060
 * Copyright (C) 2015 Powerventure Semiconductor Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __LINUX_REGULATOR_PV88060_H
#define __LINUX_REGULATOR_PV88060_H

#include <linux/regulator/machine.h>

#define PV88060_MAX_REGULATORS	14

struct pv88060_pdata {
	int num_regulator;
	struct device_node *reg_node[PV88060_MAX_REGULATORS];
	struct regulator_init_data *init_data[PV88060_MAX_REGULATORS];
};
#endif
