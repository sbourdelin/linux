/*
 * Juniper Generic Board APIs
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

#include <linux/device.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/err.h>
#include <linux/nvmem-consumer.h>
#include <linux/platform_data/at24.h>
#include <linux/slab.h>
#include <linux/jnx/jnx-subsys.h>
#include <linux/jnx/jnx-board-core.h>
#include <linux/mfd/core.h>
#include <linux/mfd/jnx-i2cs-core.h>

#include "jnx-subsys-private.h"

#define DRIVER_VERSION  "0.01.0"
#define DRIVER_DESC     "Board Generic HW"

static LIST_HEAD(jnx_i2c_notify_list);
static DEFINE_MUTEX(jnx_i2c_notify_lock);

static int jnx_i2c_adap_name_match(struct device *dev, void *data)
{
	struct i2c_adapter *adap = i2c_verify_adapter(dev);
	char *name = data;

	if (!adap)
		return false;

	return !strncmp(adap->name, name, strlen(name));
}

struct i2c_adapter *jnx_i2c_find_adapter(char *name)
{
	struct device *dev;
	struct i2c_adapter *adap;

	dev = bus_find_device(&i2c_bus_type, NULL, name,
			      jnx_i2c_adap_name_match);
	if (!dev)
		return NULL;

	adap = i2c_verify_adapter(dev);
	if (!adap)
		put_device(dev);

	return adap;
}
EXPORT_SYMBOL(jnx_i2c_find_adapter);

static void jnx_board_ideeprom_callback(struct nvmem_device *nvmem,
					void *context)
{
	struct nvmem_device **pnvmem = context;

	*pnvmem = nvmem;
}

static struct i2c_client *jnx_add_board_ideeprom(struct i2c_adapter *adap,
						 int slot)
{
	struct i2c_board_info binfo = { I2C_BOARD_INFO("24c02", 0x51) };
	struct nvmem_device *nvmem = NULL;
	struct at24_platform_data adata = {
		.byte_len = 256,
		.page_size = 1,
		.setup = jnx_board_ideeprom_callback,
		.context = &nvmem,
	};
	struct device *dev = &adap->dev;
	struct jnx_card_info cinfo = {
		.type = JNX_BOARD_TYPE_UNKNOWN,
		.adap = adap,
		.slot = slot,
	};
	struct i2c_client *client;
	unsigned char buf[2];
	int err;

	binfo.platform_data = &adata;

	client = i2c_new_device(adap, &binfo);
	if (!client)
		return client;

	if (!nvmem || nvmem_device_read(nvmem, 4, 2, buf) != 2)
		goto error;

	cinfo.assembly_id = (buf[0] << 8) | buf[1];
	err = jnx_register_board(dev, &client->dev, &cinfo, slot);
	if (err)
		goto error;

	return client;

error:
	i2c_unregister_device(client);
	return NULL;
}

/*
 * The i2cs (cpld) mux driver is instantiated through the i2cs mfd driver.
 * Provide the necessary information to the mfd driver using platform data.
 */
static struct mfd_cell i2cs_cells[] = {
	{
		.name = "i2c-mux-i2cs",
		.of_compatible = "jnx,i2c-mux-i2cs",
	},
};

static struct jnx_i2cs_platform_data i2cs_pdata = {
	.cells = i2cs_cells,
	.ncells = ARRAY_SIZE(i2cs_cells),
};

static struct i2c_board_info const jnx_i2cs_board_info = {
	I2C_BOARD_INFO("jnx_i2cs_fpc", 0x54),
	.platform_data = &i2cs_pdata,
};

struct i2c_client *jnx_board_inserted(struct i2c_adapter *adap,
				      int slot, bool has_mux)
{
	char name[JNX_BRD_I2C_NAME_LEN];
	struct i2c_client *mux, *client;

	/*
	 * First add (bus selector) mux adapter if needed
	 *
	 * Devices are connected either to the primary mux
	 * (pca9665 adapter controlled via cbd fpga mux),
	 *
	 * -[pca6996]--[cbd mux]
	 *                  +----+--- eeprom
	 *                  |    +--- other devices
	 *                  |
	 *                  +----
	 *                  ...
	 *
	 * or through the secondary mux (i2c-mux-cpld).
	 * The secondary mux is a virtual single-channel mux; its purpose
	 * is to enable i2c access to the board in question.
	 *
	 * -[pca6996]--[cbd mux]
	 *                  +----+--- eeprom
	 *                  |    +--- other devices
	 *                  |
	 *                  +----[i2c-mux-cpld]--+--- eeprom
	 *                  |                    +--- other devices
	 *                  ...
	 */
	if (has_mux) {
		mux = i2c_new_device(adap, &jnx_i2cs_board_info);
		if (!mux)
			return NULL;
		/* Look for mux adapter */
		snprintf(name, sizeof(name), "i2c-%d-mux (chan_id 0)",
			 i2c_adapter_id(adap));
		/*
		 * NOTICE:
		 * The following call will fail if the mux or the mfd driver
		 * are not built into the kernel. Accept this limitation
		 * as the code is expected to be replaced with DT based
		 * instantiation.
		 */
		adap = jnx_i2c_find_adapter(name);
		if (!adap)
			return NULL;
	}

	/* Add ideeprom either directly or behind mux */
	client = jnx_add_board_ideeprom(adap, slot);
	/*
	 * jnx_i2c_find_adapter acquires a hold on the returned adapter.
	 * Time to release it.
	 */
	if (has_mux)
		put_device(&adap->dev);
	if (has_mux && !client) {
		i2c_unregister_device(mux);
		return NULL;
	}
	return has_mux ? mux : client;
}
EXPORT_SYMBOL(jnx_board_inserted);

void jnx_board_removed(struct i2c_adapter *adap, struct i2c_client *client)
{
	/*
	 * When removing a board, we have to release the platform driver first.
	 * This is necessary because the platform driver will release the i2c
	 * devices connected to it. The 'client' variable may point to the
	 * secondary mux ('i2c-mux-cpld'). If we release it first, it would
	 * release all downstream clients, which would result in a
	 * double-release, since the platform driver would subsequently
	 * try to release the same clients again.
	 * We can not release every client from here since the platform driver
	 * may be unloaded, which would result in no release, and because
	 * the secondary mux does not exist for all boards.
	 */
	if (adap)
		jnx_unregister_board(&adap->dev);
	if (client)
		i2c_unregister_device(client);
}
EXPORT_SYMBOL(jnx_board_removed);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL v2");

/* Support kexec feature for Juniper boards:
 * 1. 'warmboot' in the command indicates a warmboot;
 * 2. jnx_warmboot API is used to check for warmboot.
 */
static bool	__jnx_warmbooted;

static int	__init jnx_warmboot_set(char *str)
{
	__jnx_warmbooted = true;
	return 0;
}

early_param("warmboot", jnx_warmboot_set);

bool
jnx_warmboot(void)
{
	return __jnx_warmbooted;
}
EXPORT_SYMBOL(jnx_warmboot);
