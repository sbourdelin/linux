/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Lochnagar internals
 *
 * Copyright (c) 2013-2018 Cirrus Logic Inc.
 *
 * Author: Charles Keepax <ckeepax@opensource.cirrus.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef CIRRUS_LOCHNAGAR_H
#define CIRRUS_LOCHNAGAR_H

#include "lochnagar1_regs.h"
#include "lochnagar2_regs.h"

struct device;
struct regmap;
struct mutex;

enum lochnagar_type {
	LOCHNAGAR1,
	LOCHNAGAR2,
};

struct lochnagar {
	enum lochnagar_type type;
	struct device *dev;
	struct regmap *regmap;

	/* Lock to protect updates to the analogue configuration */
	struct mutex analogue_config_lock;
};

/* Register Addresses */
#define LOCHNAGAR_SOFTWARE_RESET                             0x00
#define LOCHNAGAR_FIRMWARE_ID1                               0x01
#define LOCHNAGAR_FIRMWARE_ID2                               0x02

/* (0x0000)  Software Reset */
#define LOCHNAGAR_DEVICE_ID_MASK                           0xFFFC
#define LOCHNAGAR_DEVICE_ID_SHIFT                               2
#define LOCHNAGAR_REV_ID_MASK                              0x0003
#define LOCHNAGAR_REV_ID_SHIFT                                  0

int lochnagar_update_config(struct lochnagar *lochnagar);

#endif
