/*
 * pv88090.h - Regulator device driver for PV88090
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

#ifndef __LINUX_REGULATOR_PV88090_H
#define __LINUX_REGULATOR_PV88090_H

#include <linux/regulator/machine.h>

#define PV88090_MAX_REGULATORS	5

struct pv88090_pdata {
	int num_regulator;
	struct device_node *reg_node[PV88090_MAX_REGULATORS];
	struct regulator_init_data *init_data[PV88090_MAX_REGULATORS];
};
#endif
