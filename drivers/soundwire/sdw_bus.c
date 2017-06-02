/*
 *  This file is provided under a dual BSD/GPLv2 license.  When using or
 *  redistributing this file, you may do so under either license.
 *
 *  GPL LICENSE SUMMARY
 *
 *  Copyright(c) 2015-17 Intel Corporation.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of version 2 of the GNU General Public License as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 *  BSD LICENSE
 *
 *  Copyright(c) 2015-17 Intel Corporation.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
 *      distribution.
 *    * Neither the name of Intel Corporation nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/*
 * First written by Hardik T Shah
 * Rewrite by Vinod
 */


#include <linux/init.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/soundwire/soundwire.h>
#include "sdw_bus.h"

/**
 * sdw_add_bus_master: add a bus master instance
 *
 * @bus: the bus instance
 *
 * This initializes the bus instance and adds and starts reading the
 * properties for child devices and creates them
 */
int sdw_add_bus_master(struct sdw_bus *bus)
{
	if (!bus->dev) {
		pr_err("Soundwire bus w/o a device\n");
		return -ENODEV;
	}
	if (!bus->ops) {
		dev_err(bus->dev, "Bus ops are missing\n");
		return -EINVAL;
	}

	spin_lock_init(&bus->lock);
	INIT_LIST_HEAD(&bus->slaves);

	if (bus->ops->read_prop)
		bus->ops->read_prop(bus);

	sdw_sysfs_bus_init(bus);

	/*
	 * SDW is an enumerable bus, but devices can be powered off, so they
	 * won't be able to report as present.
	 *
	 * So do a device creation now, probe the driver and wait for them
	 * to report as present before using them
	 *
	 * here we need to find the slaves described in the respective
	 * firmware (ACPI/DT)
	 */

	/* ACPI check first */
	sdw_acpi_find_slaves(bus);

	return 0;
}
EXPORT_SYMBOL(sdw_add_bus_master);

void sdw_delete_bus_master(struct sdw_bus *bus)
{
	WARN_ON(!list_empty(&bus->slaves));
}
EXPORT_SYMBOL(sdw_delete_bus_master);

/*
 * SDW IO Calls
 */

static int _sdw_transfer(struct sdw_bus *bus, struct sdw_slave *slave,
				struct sdw_msg *msg, struct sdw_wait *wait)
{
	int page;
	int ret;

	/*
	 * scp paging addr is defined as:
	 *  SDW_ENUM_ADDR-> 0 we are enumerating so don't program
	 *	scp, sets to default.
	 *  SDW_BROADCAST_ADDR -> 15, its broadcast, so program SCP
	 *  Rest: dependent on paging support
	 */
	switch (msg->device) {
	case SDW_ENUM_ADDR:
		page = 0;
		break;

	case SDW_BROADCAST_ADDR:
		page = 1;
		break;

	default:
		if (slave)
			page = slave->prop.paging_support;
		else
			page = 0;

		break;
	}

	if (!wait) {
		spin_lock(&bus->lock);
		ret = bus->ops->xfer_msg(bus, msg, page);
		spin_unlock(&bus->lock);
	} else {
		if (!bus->ops->xfer_msg_async)
			return -ENOTSUPP;

		wait->msg = msg;
		wait->length = msg->len;

		spin_lock(&bus->lock);
		ret = bus->ops->xfer_msg_async(bus, msg, page, wait);
		spin_unlock(&bus->lock);

		if (ret < 0) {
			dev_err(bus->dev, "Transfer async msg failed: %d\n", ret);
			return ret;
		}
	}

	return ret;
}

/**
 * sdw_transfer: transfers messages(s) to a sdw slave device.
 * The transfer is done synchronously and this call would wait for the
 * result and return
 *
 * @bus: sdw bus
 * @slave: sdw slave, can be null for broadcast
 * @msg: the sdw message to be xfered
 */
int sdw_transfer(struct sdw_bus *bus, struct sdw_slave *slave,
					struct sdw_msg *msg)
{
	int ret;

	pm_runtime_get_sync(bus->dev);
	ret = _sdw_transfer(bus, slave, msg, NULL);
	pm_runtime_put(bus->dev);

	return ret;
}
EXPORT_SYMBOL(sdw_transfer);

/**
 * sdw_transfer_async: transfers messages(s) to a sdw slave device
 * asynchronously
 *
 * The transfer is done asynchronously and this call would return without the
 * result and caller would be signalled for completion on sdw_wait
 *
 * @bus: sdw bus
 * @slave: sdw slave, can be null for broadcast
 * @msg: the sdw message to be xfered
 * @wait: wait block on which API will signal completion
 */
int sdw_transfer_async(struct sdw_bus *bus, struct sdw_slave *slave,
			struct sdw_msg *msg, struct sdw_wait *wait)
{
	int ret;

	pm_runtime_get_sync(bus->dev);
	ret =_sdw_transfer(bus, slave, msg, wait);
	pm_runtime_put(bus->dev);

	return ret;
}
EXPORT_SYMBOL(sdw_transfer_async);

s8 sdw_read(struct sdw_slave *slave, u16 addr)
{
	struct sdw_msg msg;
	u8 buf[1];
	int ret;

	msg.addr = addr;
	msg.len = 1;
	msg.device = slave->addr;
	msg.addr_page1 = 0;
	msg.addr_page2 = 0;
	msg.flags = SDW_MSG_FLAG_READ;
	msg.buf = buf;
	msg.ssp_sync = 0;

	ret = sdw_transfer(slave->bus, slave, &msg);
	if (ret)
		return buf[0];
	else
		return ret;
}
EXPORT_SYMBOL(sdw_read);

int sdw_write(struct sdw_slave *slave, u16 addr, u8 value)
{
	struct sdw_msg msg;
	u8 buf[1];

	buf[0] = value;

	msg.addr = addr;
	msg.len = 1;
	msg.device = slave->addr;
	msg.addr_page1 = 0;
	msg.addr_page2 = 0;
	msg.flags = SDW_MSG_FLAG_WRITE;
	msg.buf = buf;
	msg.ssp_sync = 0;

	return sdw_transfer(slave->bus, slave, &msg);
}
EXPORT_SYMBOL(sdw_write);

int sdw_nread(struct sdw_slave *slave, u16 addr, size_t count, u8 *val)
{
	struct sdw_msg msg;

	msg.addr = addr;
	msg.len = count;
	msg.device = slave->addr;
	msg.addr_page1 = 0;
	msg.addr_page2 = 0;
	msg.flags = SDW_MSG_FLAG_READ;
	msg.buf = val;
	msg.ssp_sync = 0;

	return sdw_transfer(slave->bus, slave, &msg);
}
EXPORT_SYMBOL(sdw_nread);

int sdw_nwrite(struct sdw_slave *slave, u16 addr, size_t count, u8 *val)
{
	struct sdw_msg msg;

	msg.addr = addr;
	msg.len = count;
	msg.device = slave->addr;
	msg.addr_page1 = 0;
	msg.addr_page2 = 0;
	msg.flags = SDW_MSG_FLAG_WRITE;
	msg.buf = val;
	msg.ssp_sync = 0;

	return sdw_transfer(slave->bus, slave, &msg);
}
EXPORT_SYMBOL(sdw_nwrite);

static int sdw_write_nopm(struct sdw_slave *slave, u16 addr, u8 value)
{
	struct sdw_msg msg;
	u8 buf[1];

	buf[0] = value;

	msg.addr = addr;
	msg.len = 1;
	msg.device = slave->addr;
	msg.addr_page1 = 0;
	msg.addr_page2 = 0;
	msg.flags = SDW_MSG_FLAG_WRITE;
	msg.buf = buf;
	msg.ssp_sync = 0;

	return _sdw_transfer(slave->bus, slave, &msg, NULL);
}

static s8 sdw_bus_read_nopm(struct sdw_bus *bus, u16 addr)
{
	struct sdw_msg msg;
	u8 buf[1];
	int ret;

	msg.addr = addr;
	msg.len = 1;
	msg.addr_page1 = 0;
	msg.addr_page2 = 0;
	msg.flags = SDW_MSG_FLAG_READ;
	msg.buf = buf;
	msg.ssp_sync = 0;

	msg.device = SDW_BROADCAST_ADDR;
	ret = _sdw_transfer(bus, NULL, &msg, NULL);
	if (ret)
		return buf[0];
	else
		return ret;
}

static int sdw_bus_write_nopm(struct sdw_bus *bus, u16 addr, u8 value)
{
	struct sdw_msg msg;
	u8 buf[1];

	buf[0] = value;

	msg.addr = addr;
	msg.len = 1;
	msg.addr_page1 = 0;
	msg.addr_page2 = 0;
	msg.flags = SDW_MSG_FLAG_WRITE;
	msg.buf = buf;
	msg.ssp_sync = 0;
	msg.device = SDW_BROADCAST_ADDR;

	return _sdw_transfer(bus, NULL, &msg, NULL);
}
