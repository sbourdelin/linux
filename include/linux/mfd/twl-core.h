/*
 * MFD core driver for the Texas Instruments TWL PMIC family
 *
 * Copyright (C) 2016 Nicolae Rosia <nicolae.rosia@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __TWL_CORE_H__
#define __TWL_CORE_H__

/* Structure for each TWL4030/TWL6030 Slave */
struct twl_client {
	struct i2c_client *client;
	struct regmap *regmap;
};

/* mapping the module id to slave id and base address */
struct twl_mapping {
	unsigned char sid; /* Slave ID */
	unsigned char base; /* base address */
};

struct twlcore {
	bool ready; /* The core driver is ready to be used */
	u32 twl_idcode; /* TWL IDCODE Register value */
	unsigned int twl_id;

	struct twl_mapping *twl_map;
	struct twl_client *twl_modules;
};

#endif
