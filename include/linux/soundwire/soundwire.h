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

#ifndef __SOUNDWIRE_H
#define __SOUNDWIRE_H

#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/soundwire/sdw_registers.h>

struct sdw_bus;
struct sdw_slave;

/* sdw spec defines and enums, as defined by MIPI 1.1. Spec */

/* SDW Broadcast addr */
#define SDW_BROADCAST_ADDR		15

/* SDW Enumeration addr */
#define SDW_ENUM_ADDR			0

/* frame shape defines */
#define SDW_FRAME_MAX_ROWS		23
#define SDW_FRAME_MAX_COLS		8
#define SDW_FRAME_ROW_COLS		(SDW_FRAME_MAX_ROWS * SDW_FRAME_MAX_COLS)
#define SDW_FRAME_CTRL_BITS		48

#define SDW_NUM_DEV_ID_REGISTERS	6
#define SDW_MAX_DEVICES			11

enum {
	SDW_PORT_DIRN_SINK = 0,
	SDW_PORT_DIRN_SOURCE,
	SDW_PORT_DIRN_MAX,
};

/*
 * constants for flow control, ports and transport
 *
 * these are bit masks as devices can have multiple capabilities
 */

/**
 * enum sdw_slave_status: slave status
 *
 * @SDW_SLAVE_NOT_PRESENT: slave is not present on bus
 * @SDW_SLAVE_PRESENT: slave is attached to bus. This also means that salve
 * is synchronized to sdw clock
 * @SDW_SLAVE_ALERT: Some alert condition on the Slave
 */
enum sdw_slave_status {
	SDW_SLAVE_NOT_PRESENT = 0,
	SDW_SLAVE_PRESENT = 1,
	SDW_SLAVE_ALERT = 2,
	SDW_SLAVE_RESERVED = 3,
};

/**
 * enum sdw_command_response: Command response as defined by SDW spec
 *
 * @SDW_CMD_OK: cmd was okay
 * @SDW_CMD_IGNORED: cmd is ignored
 * @SDW_CMD_FAILED: cmd failed
 *
 * The enum is different than actual Spec as response in the Spec is
 * combination of ACK/NAK bits
 */
enum sdw_command_response {
	SDW_CMD_OK = 0,
	SDW_CMD_IGNORED = 1,
	SDW_CMD_FAILED = 2,
};

/*
 * sdw bus defines
 */

extern struct bus_type sdw_bus_type;

/**
 * struct sdw_slave_id: Slave ID
 *
 * @mfg_id: MIPI Manufacturing code
 * @part_id: Device Part ID
 * @class_id: MIPI Class ID
 * @unique_id: Device unique ID
 * @sdw_version: SDW version implemented
 * @link_id: link instance number
 */
struct sdw_slave_id {
	__u16 mfg_id;
	__u16 part_id;
	__u8 class_id;
	__u8 unique_id:4;
	__u8 sdw_version:4;
	__u16 link_id;
};

/**
 * struct sdw_slave: SoundWire Slave
 *
 * @id: MIPI device ID
 * @dev: Linux device
 * @status: device enumeration status
 * @bus: bus for this slave
 * @node: node for bus list of slaves
 * @addr: Logical address
 */
struct sdw_slave {
	struct sdw_slave_id id;
	struct device dev;
	enum sdw_slave_status status;
	struct sdw_bus *bus;
	const struct sdw_slave_ops *ops;
	struct sdw_slave_sysfs *sysfs;
	struct list_head node;
	u16 addr;
};

#define dev_to_sdw_dev(_dev) container_of(_dev, struct sdw_slave, dev)

struct sdw_msg;
struct sdw_wait;

/**
 * struct sdw_master_ops: master driver ops
 *
 * @read_prop: read the properties of a master
 * @xfer_msg: the transfer message callback
 * @xfer_msg_async: the async version of transfer message callback
 */
struct sdw_master_ops {
	enum sdw_command_response (*xfer_msg)
			(struct sdw_bus *bus, struct sdw_msg *msg, int page);
	enum sdw_command_response (*xfer_msg_async)
			(struct sdw_bus *bus, struct sdw_msg *msg,
			int page, struct sdw_wait *wait);
};

/**
 * struct sdw_msg: Message to be sent on sdw bus
 *
 * @addr: the register address of the slave
 * @len: number of messages i.e reads/writes to be performed
 * @addr_page1: SCP address page 1 Slave register
 * @addr_page2: SCP address page 2 Slave register
 * @flags: transfer flags, indicate if xfer is read or write
 * @buf: message data buffer
 * @ssp_sync: Send message at SSP (Stream Synchronization Point)
 */
struct sdw_msg {
	u16 addr;
	u16 len;
	u16 device;
	u8 addr_page1;
	u8 addr_page2;
	u8 flags;
	u8 *buf;
	bool ssp_sync;
};

struct sdw_wait {
	int length;
	struct completion complete;
	struct sdw_msg *msg;
};

/**
 * struct sdw_bus: the SoundWire bus
 *
 * @bus_node: Node for bus list
 * @dev: master device
 * @acpi_enabled: is this bus acpi enabled or not
 * @link_id: Link id number, can be 0 to N
 * @slaves: list of slaves on this bus
 * @assigned: logical addresses assigned
 * @lock: bus lock
 * @ops: master callback ops
 * @wait_msg: wait messages for async messages
 */
struct sdw_bus {
	struct list_head bus_node;
	struct device *dev;
	bool acpi_enabled;
	unsigned int link_id;
	struct list_head slaves;
	bool assigned[SDW_MAX_DEVICES + 1];
	spinlock_t lock;
	const struct sdw_master_ops *ops;
	struct sdw_wait wait_msg;
};

int sdw_add_bus_master(struct sdw_bus *bus);
void sdw_delete_bus_master(struct sdw_bus *bus);

struct sdw_driver {
	const char *name;

	int (*probe)(struct sdw_slave *sdw,
			const struct sdw_device_id *id);
	int (*remove)(struct sdw_slave *sdw);
	void (*shutdown)(struct sdw_slave *sdw);

	const struct sdw_device_id *id_table;
	const struct sdw_slave_ops *ops;

	struct device_driver driver;
};

#define drv_to_sdw_driver(_drv) container_of(_drv, struct sdw_driver, driver)

int sdw_register_driver(struct sdw_driver *drv, struct module *owner);
void sdw_unregister_driver(struct sdw_driver *drv);

/* messaging and data api's */

s8 sdw_read(struct sdw_slave *slave, u16 addr);
int sdw_write(struct sdw_slave *slave, u16 addr, u8 value);
int sdw_nread(struct sdw_slave *slave, u16 addr, size_t count, u8 *val);
int sdw_nwrite(struct sdw_slave *slave, u16 addr, size_t count, u8 *val);

enum {
	SDW_MSG_FLAG_READ = 0,
	SDW_MSG_FLAG_WRITE,
};

int sdw_transfer(struct sdw_bus *bus, struct sdw_slave *slave,
			struct sdw_msg *msg);
int sdw_transfer_async(struct sdw_bus *bus, struct sdw_slave *slave,
			struct sdw_msg *msg, struct sdw_wait *wait);

 #endif /* __SOUNDWIRE_H */
