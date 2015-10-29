/*
 * Copyright (c) 2015 - Savoir-faire Linux
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef __LINUX_MFD_TS4800_SYSCON_H
#define __LINUX_MFD_TS4800_SYSCON_H

#include <linux/regmap.h>

struct ts4800_syscon {
	struct regmap		*regmap;
};

#endif /* __LINUX_MFD_TS4800_SYSCON_H */
