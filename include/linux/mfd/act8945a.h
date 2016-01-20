/*
 * MFD driver for Active-semi ACT8945a PMIC
 *
 * Copyright (C) 2015 Atmel Corporation.
 *
 * Author: Wenyou Yang <wenyou.yang@atmel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under  the terms of the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef _LINUX_MFD_ACT8945A_H
#define _LINUX_MFD_ACT8945A_H

struct act8945a_dev {
	struct regmap *regmap;
};

#endif
