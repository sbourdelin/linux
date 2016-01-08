/*
 * MFD for Active-semi ACT8945A PMIC
 *
 * Copyright (C) 2015 Atmel Corporation.
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

#ifndef _LINUX_MFD_ACT8945A_H
#define _LINUX_MFD_ACT8945A_H

struct act8945a_dev {
	struct device *dev;
	struct i2c_client *i2c_client;
	struct regmap *regmap;
};

#endif
