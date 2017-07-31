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

#ifndef I3C_INTERNALS_H
#define I3C_INTERNALS_H

#include <linux/i3c/master.h>

extern struct bus_type i3c_bus_type;
extern const struct device_type i3c_master_type;
extern const struct device_type i3c_device_type;

void i3c_bus_destroy(struct i3c_bus *bus);
struct i3c_bus *i3c_bus_create(struct device *parent);
void i3c_bus_unregister(struct i3c_bus *bus);
int i3c_bus_register(struct i3c_bus *i3cbus);
void i3c_bus_lock(struct i3c_bus *bus, bool exclusive);
void i3c_bus_unlock(struct i3c_bus *bus, bool exclusive);
int i3c_bus_get_free_addr(struct i3c_bus *bus, u8 start_addr);
bool i3c_bus_dev_addr_is_avail(struct i3c_bus *bus, u8 addr);
void i3c_bus_set_addr_slot_status(struct i3c_bus *bus, u16 addr,
				  enum i3c_addr_slot_status status);
enum i3c_addr_slot_status i3c_bus_get_addr_slot_status(struct i3c_bus *bus,
						       u16 addr);

int i3c_master_do_priv_xfers_locked(struct i3c_master_controller *master,
				    const struct i3c_priv_xfer *xfers,
				    int nxfers);
int i3c_master_send_hdr_cmds_locked(struct i3c_master_controller *master,
				    const struct i3c_hdr_cmd *cmds, int ncmds);

#endif /* I3C_INTERNAL_H */
