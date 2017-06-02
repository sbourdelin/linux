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

/*
 * Global SoundWire core instance contains list of Masters registered, core
 *	lock and SoundWire stream tags.
 */
struct sdw_core sdw_core;

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
	INIT_LIST_HEAD(&bus->mstr_rt_list);

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

	/* Initialize bandwidth calculation data structures */
	sdw_init_bus_params(bus);

	/*
	 * Add bus to the list of buses inside core. This is list of Slave
	 * devices enumerated on this bus. Adding new devices at end. It can
	 * be added at any location in list.
	 */
	list_add_tail(&bus->bus_node, &sdw_core.bus_list);


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

/*
 * SDW alert handling
 */

static struct sdw_slave *sdw_get_slave(struct sdw_bus *bus, int i)
{
	struct sdw_slave *slave;

	list_for_each_entry(slave, &bus->slaves, node) {
		if (slave->addr == i)
			return slave;
	}
	return NULL;
}

static int sdw_compare_devid(struct sdw_slave *slave, struct sdw_slave_id id)
{
	if (slave->id.unique_id != id.unique_id)
		return -ENODEV;
	if (slave->id.mfg_id != id.mfg_id)
		return -ENODEV;
	if (slave->id.part_id != id.part_id)
		return -ENODEV;
	if (slave->id.class_id != id.class_id)
		return -ENODEV;

	return 0;
}

static int sdw_get_logical_addr(struct sdw_slave *slave)
{
	int i;
	bool assigned = false;

	spin_lock(&slave->bus->lock);

	for (i = 1; i < SDW_MAX_DEVICES; i++) {
		if (slave->bus->assigned[i] == true)
			continue;

		slave->bus->assigned[i] = true;
		slave->addr = i;
		assigned = true;
		break;
	}

	spin_unlock(&slave->bus->lock);

	if (assigned)
		return i;
	else
		return -ENODEV;
}

static int sdw_assign_logical_addr(struct sdw_slave *slave)
{
	int ret;

	ret = sdw_get_logical_addr(slave);
	if (ret <= 0) {
		dev_err(&slave->dev, "Finding empty LA failed: %d\n", ret);
		return ret;
	}

	ret = sdw_write(slave, SDW_SCP_DEVNUMBER, ret);
	if (ret != 1) {
		dev_err(&slave->dev, "Program LA failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static int sdw_program_logical_addr(struct sdw_bus *bus)
{
	struct sdw_msg msg;
	u8 buf[SDW_NUM_DEV_ID_REGISTERS] = {0};
	struct sdw_slave_id id;
	struct sdw_slave *slave;
	bool found = false;
	int ret;

	/* read all the devices reporting first */

	/* we dont have slave, so use raw xfer api */

	msg.addr = SDW_SCP_DEVID_0;
	msg.len = SDW_NUM_DEV_ID_REGISTERS;
	msg.device = 0;
	msg.addr_page1 = 0;
	msg.addr_page2 = 0;
	msg.flags = SDW_MSG_FLAG_READ;
	msg.buf = buf;
	msg.ssp_sync = 0;

	while ((ret = sdw_transfer(bus, NULL, &msg)) == 1) {

		/* extract the id */
		id.sdw_version = buf[0] & GENMASK(7, 4) >> 4;
		id.unique_id = buf[0] & GENMASK(3, 0);
		id.mfg_id = buf[1] | (buf[2] << 8);
		id.part_id = buf[3] | (buf[4] << 8);
		id.class_id = buf[5];

		/* now compare with entries */
		list_for_each_entry(slave, &bus->slaves, node) {
			if (0 == sdw_compare_devid(slave, id)) {
				found = true;

				/*
				 * here we assign a new LA to this slave
				 * but we won't mark it present, it will be
				 * done once it reports present on new LA
				 */
				sdw_assign_logical_addr(slave);
			}
		}

		if (found == false) {
			/*
			 * this means we don't have this device
			 * so we should assign group 13 - TODO
			 */
		}
	}

	return 0;
}

static int sdw_attach_slave(struct sdw_slave *slave)
{
	spin_lock(&slave->bus->lock);
	slave->status = SDW_SLAVE_PRESENT;
	spin_unlock(&slave->bus->lock);

	return 0;
}

static int sdw_detach_slave(struct sdw_slave *slave)
{
	spin_lock(&slave->bus->lock);
	slave->status = SDW_SLAVE_NOT_PRESENT;
	spin_unlock(&slave->bus->lock);

	return 0;
}

static enum sdw_clk_stop_mode sdw_get_clk_stop_mode(struct sdw_slave *slave)
{
	enum sdw_clk_stop_mode mode;

	/*
	 * check if driver supports query, if so query it
	 * if not, read from properties
	 */
	if (slave->ops->get_clk_stop_mode)
		mode = slave->ops->get_clk_stop_mode(slave);
	else
		mode = SDW_CLK_STOP_MODE1;

	return mode;
}

static int sdw_slave_pre_clk_stop(struct sdw_slave *slave,
		enum sdw_clk_stop_mode mode, bool prepare)
{
	bool wake_en;
	u32 val = 0;
	enum sdw_clok_stop_type type;
	int ret;

	if (prepare)
		type = SDW_CLK_PRE_STOP;
	else
		type = SDW_CLK_PRE_START;

	if (slave->ops->clk_stop) {
		ret = slave->ops->clk_stop(slave, mode, type);
		if (ret)
			dev_warn(&slave->dev, "Pre Clk Stop failed: %d\n", ret);
	}

	wake_en = slave->prop.wake_capable;

	if (prepare) {
		/*
		 * Set prepare bits even for simplified clock stop prepare,
		 * its safer to do so and harmless :)
		 */
		val = SDW_SCP_SYSTEMCTRL_CLK_STP_PREP;
		if (mode == SDW_CLK_STOP_MODE1)
			val |= SDW_SCP_SYSTEMCTRL_CLK_STP_MODE;
		if (wake_en)
			val |= SDW_SCP_SYSTEMCTRL_WAKE_UP_EN;
	}

	/*
	 * this fn is invoked from pm handler of master so we need
	 * nopm variant for write
	 */
	sdw_write_nopm(slave, SDW_SCP_SYSTEMCTRL, val);

	return 0;
}

static int sdw_bus_wait_for_clk_prep(struct sdw_bus *bus)
{
	int val;
	int retry = 20;

	/*
	 * Read slaves (broadcast) for STAT register to read
	 * ClockStopNotFinished, once all are clear we can proceed
	 */

	do {
		val = sdw_bus_read_nopm(bus, SDW_SCP_STAT) & SDW_SCP_STAT_CLK_STP_NF;
		if (!val)
			break;

		udelay(20);
		retry--;
	} while (retry);

	if (retry)
		dev_info(bus->dev, "clock stop prepare done\n");
	else
		dev_err_ratelimited(bus->dev, "clock stop prepare failed\n");

	return 0;
}

/**
 * sdw_bus_prep_clk_stop: prepare the slaves for clock stop
 *
 * @bus: the sdw bus instance
 *
 * All the slaves marking themselves as present on the bus are prepare for
 * stopping the clock.
 *
 * The slave tells us which clock stop mode it wants (if not supported
 * fallback to property value) and set that up. This also invokes driver
 * before and after preparing for clock stop.
 */
int sdw_bus_prep_clk_stop(struct sdw_bus *bus)
{
	struct sdw_slave *slave;
	enum sdw_clk_stop_mode mode = SDW_CLK_STOP_MODE1;

	list_for_each_entry(slave, &bus->slaves, node) {
		if (slave->status == SDW_SLAVE_PRESENT) {
			/* call pre clock stop, if it is supported */
			mode = sdw_get_clk_stop_mode(slave);

			/* call driver for pre clk stop */
			sdw_slave_pre_clk_stop(slave, mode, true);
		}
	}

	sdw_bus_wait_for_clk_prep(bus);

	/* tell slaves that prep is done */
	list_for_each_entry(slave, &bus->slaves, node) {
		if (slave->status == SDW_SLAVE_PRESENT && slave->ops->clk_stop)
			slave->ops->clk_stop(slave, mode, SDW_CLK_POST_STOP);
	}

	return 0;
}
EXPORT_SYMBOL(sdw_bus_prep_clk_stop);

/**
 * sdw_bus_clk_stop: stop the bus clock
 *
 * @bus: the sdw bus instance
 *
 * After preparing the slaves for clock stop, we stop the clock here
 * This is done by broadcasting write to SCP_CTRL register.
 */
int sdw_bus_clk_stop(struct sdw_bus *bus)
{
	struct sdw_slave *slave;
	enum sdw_clk_stop_mode mode;
	int ret;

	/*
	 * broadcast clock stop now, attached slaves will ACK this,
	 * unattached will ignore
	 *
	 * continue even if we get err
	 */
	ret = sdw_bus_write_nopm(bus, SDW_SCP_CTRL, SDW_SCP_CTRL_CLK_STP_NOW);
	if (ret)
		dev_err(bus->dev, "ClockStopNow Broadcast message failed\n");

	/* now mark slaves entering clock stop as unattached */
	list_for_each_entry(slave, &bus->slaves, node) {
		mode = sdw_get_clk_stop_mode(slave);
		if (mode == SDW_CLK_STOP_MODE0)
			continue;

		sdw_detach_slave(slave);
	}

	return 0;
}
EXPORT_SYMBOL(sdw_bus_clk_stop);

/**
 * sdw_bus_clk_stop_exit: exit the clock stop mode
 *
 * @bus: the sdw bus instance
 *
 * This De-prepares the Slaves by exiting Clock Stop Mode 0 as clock would
 * have resumed. For the Slaves in Clock Stop Mode 1, they will be
 * de-prepared after they enumerate back.
 */
int sdw_bus_clk_stop_exit(struct sdw_bus *bus)
{
	struct sdw_slave *slave;
	enum sdw_clk_stop_mode mode;

	list_for_each_entry(slave, &bus->slaves, node) {
		if (slave->status == SDW_SLAVE_PRESENT) {
			/* call pre clock stop, if it is supported */
			mode = sdw_get_clk_stop_mode(slave);
			if (mode == SDW_CLK_STOP_MODE1)
				continue;

			sdw_slave_pre_clk_stop(slave, mode, false);
		}
	}

	sdw_bus_wait_for_clk_prep(bus);

	list_for_each_entry(slave, &bus->slaves, node) {
		if (slave->status == SDW_SLAVE_PRESENT) {
			mode = sdw_get_clk_stop_mode(slave);
			if (mode == SDW_CLK_STOP_MODE1)
				continue;

			if (slave->ops->clk_stop)
				slave->ops->clk_stop(slave,
						mode, SDW_CLK_POST_START);
		}
	}
	return 0;
}
EXPORT_SYMBOL(sdw_bus_clk_stop_exit);

int sdw_configure_dpn_intr(struct sdw_slave *slave, int port,
					bool enable, int mask)
{
	u8 val;
	int addr, port_num;

	port_num = SDW_REG_SHIFT(port);
	addr = SDW_DPN_INTMASK + SDW_NUM_DATA_PORT_REGISTERS * port_num;

	val = sdw_read(slave, addr);
	val |= mask;

	/* Set the port ready and Test fail interrupt mask */
	val |= SDW_DPN_INT_TEST_FAIL | SDW_DPN_INT_PORT_READY;

	sdw_write(slave, addr, val);

	return 0;
}

static int sdw_programme_slave(struct sdw_slave *slave)
{
	u8 val;
	u32 bit;
	unsigned long addr;
	struct sdw_slave_prop *prop = &slave->prop;

	/* Enable DP0 and SCP interrupts */
	val = sdw_read(slave, SDW_SCP_INTMASK1);

	/* Set port read and test fail interrupt mask */
	val |= SDW_SCP_INT1_BUS_CLASH | SDW_SCP_INT1_PARITY;

	sdw_write(slave, SDW_SCP_INTMASK1, val);

	/* No need to continue if DP0 is not present */
	if (!slave->prop.dp0_prop)
		return 0;

	val = sdw_read(slave, SDW_DP0_INT_MASK);
	val |= prop->dp0_prop->device_interrupts;
	val |= SDW_DP0_INT_TEST_FAIL | SDW_DP0_INT_PORT_READY |
					SDW_DP0_INT_BRA_FAILURE;

	sdw_write(slave, SDW_DP0_INT_MASK, val);

	/* enable DPn ports */
	addr = slave->prop.source_ports;
	for_each_set_bit(bit, &addr, 32)
		sdw_configure_dpn_intr(slave, bit, true,
				prop->src_dpn_prop->device_interrupts);

	addr = slave->prop.sink_ports;
	for_each_set_bit(bit, &addr, 32)
		sdw_configure_dpn_intr(slave, bit, true,
				prop->src_dpn_prop->device_interrupts);

	return 0;
}

static int sdw_deprepare_clk_stp1(struct sdw_slave *slave)
{
	u8 val;
	int retry  = 10;

	/*
	 * first check if the slave needs deprep by checking property
	 * "mipi-sdw-clockstopprepare-hard-reset-behavior" and proceed only
	 * if this is supported
	 */
	if (!slave->prop.clk_stop_mode1)
		return 0;

	/* check first if slave requires de-prep */
	val = sdw_read(slave, SDW_SCP_SYSTEMCTRL);
	if (!(val & SDW_SCP_SYSTEMCTRL_CLK_STP_PREP))
		return 0;

	/* call driver clock stop */
	sdw_slave_pre_clk_stop(slave, SDW_CLK_STOP_MODE1, false);

	/* Wait till de-prepare is complete by checking NF bit */
	do {
		val = sdw_read(slave, SDW_SCP_STAT) & SDW_SCP_STAT_CLK_STP_NF;
		if (!val)
			break;

		udelay(20);
		retry--;
	} while (retry);

	if (retry)
		dev_info(&slave->dev, "clock stop prepare done\n");
	else
		dev_err_ratelimited(&slave->dev, "clock stop prepare failed\n");

	/* again tell driver */
	if (slave->ops->clk_stop)
		slave->ops->clk_stop(slave,
				SDW_CLK_STOP_MODE1, SDW_CLK_POST_START);

	return 0;
}

static int sdw_handle_dp0_interrupt(struct sdw_slave *slave, u8 *slave_status)
{
	u8 status, clear = 0, impl_int_mask;

	/* read DP0 interrupts */
	status = sdw_read(slave, SDW_DP0_INT);

	if (status & SDW_DP0_INT_TEST_FAIL) {
		dev_err(&slave->dev, "Test fail for port 0\n");
		clear |= SDW_DP0_INT_TEST_FAIL;
	}

	if (status & SDW_DP0_INT_PORT_READY) {
		//completion of port here ??
		clear |= SDW_DP0_INT_PORT_READY;
	}

	if (status & SDW_DP0_INT_BRA_FAILURE) {
		dev_err(&slave->dev, "BRA failed\n");
		clear |= SDW_DP0_INT_BRA_FAILURE;
	}

	impl_int_mask = SDW_DP0_INT_IMPDEF1 |
		SDW_DP0_INT_IMPDEF2 | SDW_DP0_INT_IMPDEF3;

	if (status & impl_int_mask) {
		clear |= impl_int_mask;
		*slave_status = impl_int_mask;
	}

	/* clear the interrupt */
	sdw_write(slave, SDW_DP0_INT, clear);

	return 0;
}

static int sdw_handle_port_interrupt(struct sdw_slave *slave,
		int port, u8 *slave_status)
{
	int addr;
	u8 status, clear = 0, impl_int_mask;

	if (port  == 0)
		return sdw_handle_dp0_interrupt(slave, slave_status);

	addr = SDW_DPN_INT + SDW_NUM_DATA_PORT_REGISTERS * port;
	status = sdw_read(slave, addr);

	if (status & SDW_DPN_INT_TEST_FAIL) {
		dev_err(&slave->dev, "Test fail for port: %d\n", port);
		clear |= SDW_DPN_INT_TEST_FAIL;
	}

	if (status & SDW_DPN_INT_PORT_READY) {
		//completion of port here ??
		clear |= SDW_DPN_INT_PORT_READY;
	}

	impl_int_mask = SDW_DPN_INT_IMPDEF1 |
		SDW_DPN_INT_IMPDEF2 | SDW_DPN_INT_IMPDEF3;
	if (status & impl_int_mask) {
		clear |= impl_int_mask;
		*slave_status = impl_int_mask;
	}

	/* clear the interrupt */
	sdw_write(slave, addr, clear);

	return 0;
}

static int sdw_handle_slave_alerts(struct sdw_slave *slave)
{
	u8 buf[3];
	u8 clear = 0, bit, port_status[15];
	int port_num;
	unsigned long port;

	spin_lock(&slave->bus->lock);
	slave->status = SDW_SLAVE_ALERT;
	spin_unlock(&slave->bus->lock);

	/* Read Instat 1, Instat 2 and Instat 3 registers */
	sdw_nread(slave, SDW_SCP_INT1, 3, buf);

	/* check parity, bus clash and slave (impl defined)  interrupt */
	if (buf[0] & SDW_SCP_INT1_PARITY) {
		dev_err(&slave->dev, "Parity error detected\n");
		clear |= SDW_SCP_INT1_PARITY;
	}

	if (buf[0] & SDW_SCP_INT1_BUS_CLASH) {
		dev_err(&slave->dev, "Bus clash error detected\n");
		clear |= SDW_SCP_INT1_BUS_CLASH;
	}

	if (buf[0] & SDW_SCP_INT1_IMPL_DEF) {
		dev_dbg(&slave->dev, "Slave interrupt\n");
		clear |= SDW_SCP_INT1_IMPL_DEF;

		/* notify driver here */
	}

	/* check port 0 - 4 interrupts */
	port = buf[0] & SDW_SCP_INT1_PORT0_3_MASK;

	/* to get port number corresponding to bits, shift it */
	port = port >> SDW_SCP_INT1_PORT0_3_SHIFT;
	for_each_set_bit(bit, &port, 8)
		sdw_handle_port_interrupt(slave, bit, &port_status[bit]);

	/* check if cascade 2 interrupt is present */
	if (buf[0] & SDW_SCP_INT1_SCP2_CASCADE) {
		port = buf[1] & SDW_SCP_INTSTAT2_PORT4_10_MASK;
		for_each_set_bit(bit, &port, 8) {
			/* scp2 ports start from 4 */
			port_num = bit + 3;
			sdw_handle_port_interrupt(slave,
					port_num, &port_status[port_num]);
		}
	}

	/* now check last cascade */
	if (buf[1] & SDW_SCP_INTSTAT2_SCP3_CASCADE) {
		port = buf[2] & SDW_SCP_INTSTAT3_PORT11_14_MASK;
		for_each_set_bit(bit, &port, 8) {
			/* scp3 ports start from 11 */
			port_num = bit + 10;
			sdw_handle_port_interrupt(slave,
					port_num, &port_status[port_num]);
		}
	}

	/* update the slave driver */
	if (slave->ops->interrupt_callback) {
		struct sdw_slave_intr_status slave_intr;

		slave_intr.control_port = clear;
		memcpy(slave_intr.port, &port_status, sizeof(slave_intr.port));

		slave->ops->interrupt_callback(slave, &slave_intr);
	}

	/* ack the interrupt */
	sdw_write(slave, SDW_SCP_INT1, clear);

	return 0;
}

static int sdw_update_slave_status(struct sdw_slave *slave,
				enum sdw_slave_status status)
{
	if (slave->ops->update_status)
		slave->ops->update_status(slave, status);

	return 0;
}

/**
 * sdw_handle_slave_status: handle the slave interrupts
 *
 * @bus: sdw bus
 * @status: interrupt status for each slave
 *
 * Read the status of each slave and process them by updating slave status
 * reported or in case of alert telling the driver about it
 */
int sdw_handle_slave_status(struct sdw_bus *bus,
			enum sdw_slave_status status[])
{
	struct sdw_slave *slave;
	int i, ret = 0;

	if (status[0] == SDW_SLAVE_PRESENT) {
		ret = sdw_program_logical_addr(bus);
		if (ret)
			dev_err(bus->dev, "Slave attach failed: %d\n", ret);

		/* we still continue here checking other status */
	}

	for (i = 1; i <= SDW_MAX_DEVICES; i++) {

		if (bus->assigned[i] == false)
			continue;

		slave = sdw_get_slave(bus, i);

		switch(status[i]) {
		case SDW_SLAVE_NOT_PRESENT:
			/* slave is detached now */
			sdw_detach_slave(slave);
			break;

		case SDW_SLAVE_ALERT:
			/* Handle slave alerts */
			ret = sdw_handle_slave_alerts(slave);
			if (ret)
				dev_err(bus->dev,
					"Slave %d alert handling failed: %d\n",
					i, ret);
			break;

		case SDW_SLAVE_PRESENT:

			sdw_programme_slave(slave);
			sdw_attach_slave(slave);
			sdw_deprepare_clk_stp1(slave);
			break;

		default:
			dev_err(bus->dev, "Bad status: %d\n", status[i]);
		}

		sdw_update_slave_status(slave, status[i]);
	}

	return 0;
}
EXPORT_SYMBOL(sdw_handle_slave_status);
