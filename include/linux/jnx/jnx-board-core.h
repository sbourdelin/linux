/*
 * Juniper Generic Board APIs
 *
 * Copyright (C) 2012, 2013 Juniper Networks. All rights reserved.
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

#ifndef _JNX_BOARD_CORE_H
#define _JNX_BOARD_CORE_H

/*
 * Generic Juniper board I2C bus notification list handling
 */
struct jnx_board_i2c_entry {
	struct i2c_board_info *board_info;
	int bi_num;
	char name[JNX_BRD_I2C_NAME_LEN];
	struct work_struct work;
	unsigned long action;
	struct device *dev;
	struct list_head list;
};

struct i2c_adapter *jnx_i2c_find_adapter(char *name);
struct i2c_client *jnx_board_inserted(struct i2c_adapter *adap, int slot,
				      bool has_mux);
void jnx_board_removed(struct i2c_adapter *adap, struct i2c_client *client);

/*	API testing warmboot: != 0: warmboot, == 0: coldboot */
bool	jnx_warmboot(void);

#endif /* _JNX_BOARD_CORE_H */
