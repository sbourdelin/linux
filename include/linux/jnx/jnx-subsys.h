/*
 * Juniper Generic APIs for providing chassis and card information
 *
 * Copyright (C) 2012, 2013, 2014 Juniper Networks. All rights reserved.
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

#ifndef _JNX_SUBSYS_H
#define _JNX_SUBSYS_H

#include <uapi/linux/jnx/jnx-subsys.h>

/*
 * Juniper Product Number Definitions
 */
#define JNX_PRODUCT_HERCULES	7
#define JNX_PRODUCT_SANGRIA	85
#define JNX_PRODUCT_TINY	134
#define JNX_PRODUCT_HENDRICKS	156
#define JNX_PRODUCT_POLARIS	171
#define JNX_PRODUCT_OMEGA	181

#define JNX_BRD_I2C_NAME_LEN	24

/* create and delete a link in jnx/card/<link> pointing to given dev */
int jnx_sysfs_create_link(struct device *dev, const char *link);
void jnx_sysfs_delete_link(struct device *dev, const char *link);

/**
 * struct jnx_card_info - juniper board per card information
 * @assembly_id:	assembly ID read from the EEPROM
 * @slot:		slot number in the chassis
 * @type:		type of card; see uapi jnx-subsys.h
 * @data:		per card user data
 * @adap:		pointer to the i2c_adapter EEPROM is on
 *
 * This structure contains information that each juniper board
 * provides.
 */
struct jnx_card_info {
	u16 assembly_id;
	int slot;
	u32 type;
	void *data;
	struct i2c_adapter *adap;
};

/* register and unregister a local jnx card */
int jnx_register_local_card(struct jnx_card_info *cinfo);
void jnx_unregister_local_card(void);

/**
 * struct jnx_chassis_info	- juniper chassis info and method callbacks
 * @platform:			platform id of the chassis
 * @chassis_no:			chassis number - 0 if non-multi chassis
 * @multichassis:		non zero if a multichassis system
 * @master_data:		per chassis data
 * @get_master:			get slot number of master
 * @mastership_get:		returns whether we're master
 * @mastership_set:		Relinquish mastership
 * @mastership_ping:		update mastership watchdog
 * @mastership_count_get:	get mastership watchdog counter
 * @mastership_count_set:	set mastership watchdog counter
 *
 * This structure contains per chassis information and method callbacks that
 * handle the per-platform CBD FPGA differences.
 */
struct jnx_chassis_info {
	u32 platform;
	u32 chassis_no;
	u32 multichassis;
	void *master_data;
	int (*get_master)(void *data);
	bool (*mastership_get)(void *data);
	void (*mastership_set)(void *data, bool mastership);
	void (*mastership_ping)(void *data);
	int (*mastership_count_get)(void *data);
	int (*mastership_count_set)(void *data, int val);
};

/* register and unregsiter a juniper chassis */
int jnx_register_chassis(struct jnx_chassis_info *chinfo);
void jnx_unregister_chassis(void);

#endif /* _JNX_SUBSYS_H */
