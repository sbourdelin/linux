/*
 * Copyright (C) 2017 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@free-electrons.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include "internals.h"

/**
 * i3c_device_do_priv_xfers() - do I3C SDR private transfers directed to a
 *				specific device
 *
 * @dev: device with which the transfers should be done
 * @xfers: array of transfers
 * @nxfers: number of transfers
 *
 * Initiate one or several private SDR transfers with @dev.
 *
 * This function can sleep and thus cannot be called in atomic context.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_do_priv_xfers(struct i3c_device *dev,
			     struct i3c_priv_xfer *xfers,
			     int nxfers)
{
	struct i3c_master_controller *master;
	int i, ret;

	master = i3c_device_get_master(dev);
	if (!master)
		return -EINVAL;

	i3c_bus_lock(master->bus, false);
	for (i = 0; i < nxfers; i++)
		xfers[i].addr = dev->info.dyn_addr;

	ret = i3c_master_do_priv_xfers_locked(master, xfers, nxfers);
	i3c_bus_unlock(master->bus, false);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_do_priv_xfers);

/**
 * i3c_device_send_hdr_cmds() - send HDR commands to a specific device
 *
 * @dev: device to which these commands should be sent
 * @cmds: array of commands
 * @ncmds: number of commands
 *
 * Send one or several HDR commands to @dev.
 *
 * This function can sleep and thus cannot be called in atomic context.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_send_hdr_cmds(struct i3c_device *dev,
			     struct i3c_hdr_cmd *cmds,
			     int ncmds)
{
	struct i3c_master_controller *master;
	enum i3c_hdr_mode mode;
	int ret, i;

	if (ncmds < 1)
		return 0;

	mode = cmds[0].mode;
	for (i = 1; i < ncmds; i++) {
		if (mode != cmds[i].mode)
			return -EINVAL;
	}

	master = i3c_device_get_master(dev);
	if (!master)
		return -EINVAL;

	i3c_bus_lock(master->bus, false);
	for (i = 0; i < ncmds; i++)
		cmds[i].addr = dev->info.dyn_addr;

	ret = i3c_master_send_hdr_cmds_locked(master, cmds, ncmds);
	i3c_bus_unlock(master->bus, false);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_send_hdr_cmds);

void i3c_device_get_info(struct i3c_device *dev,
			 struct i3c_device_info *info)
{
	if (info)
		*info = dev->info;
}
EXPORT_SYMBOL_GPL(i3c_device_get_info);

/**
 * i3c_driver_register_with_owner() - register an I3C device driver
 *
 * @drv: driver to register
 * @owner: module that owns this driver
 *
 * Register @drv to the core.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_driver_register_with_owner(struct i3c_driver *drv, struct module *owner)
{
	drv->driver.owner = owner;
	drv->driver.bus = &i3c_bus_type;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(i3c_driver_register_with_owner);

/**
 * i3c_driver_unregister() - unregister an I3C device driver
 *
 * @drv: driver to unregister
 *
 * Unregister @drv.
 */
void i3c_driver_unregister(struct i3c_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(i3c_driver_unregister);
