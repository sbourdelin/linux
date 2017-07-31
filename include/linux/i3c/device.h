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

#ifndef I3C_DEV_H
#define I3C_DEV_H

#include <linux/device.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>

/**
 * enum i3c_hdr_mode - HDR mode ids
 */
enum i3c_hdr_mode {
	I3C_HDR_DDR,
	I3C_HDR_TSP,
	I3C_HDR_TSL,
};

/**
 * struct i3c_hdr_cmd - I3C HDR command
 *
 * @mode: HDR mode selected for this command
 * @addr: I3C dynamic address
 * @ndatawords: number of data words (a word is 16bits wide)
 * @data: input/output buffer
 */
struct i3c_hdr_cmd {
	enum i3c_hdr_mode mode;
	u8 code;
	u8 addr;
	int ndatawords;
	union {
		u16 *in;
		const u16 *out;
	} data;
};

/* Private SDR read transfer */
#define I3C_PRIV_XFER_READ		BIT(0)
/*
 * Instruct the controller to issue a STOP after a specific transfer instead
 * of a REPEATED START.
 */
#define I3C_PRIV_XFER_STOP		BIT(1)

/**
 * struct i3c_priv_xfer - I3C SDR private transfer
 *
 * @addr: I3C dynamic address
 * @len: transfer length in bytes of the transfer
 * @flags: combination of I3C_PRIV_XFER_xxx flags
 * @data: input/output buffer
 */
struct i3c_priv_xfer {
	u8 addr;
	u16 len;
	u32 flags;
	struct {
		void *in;
		const void *out;
	} data;
};

/**
 * enum i3c_dcr - I3C DCR values
 */
enum i3c_dcr {
	I3C_DCR_GENERIC_DEVICE = 0,
};

#define I3C_PID_MANUF_ID(pid)		(((pid) & GENMASK_ULL(47, 33)) >> 33)
#define I3C_PID_RND_LOWER_32BITS(pid)	(!!((pid) & BIT_ULL(32)))
#define I3C_PID_RND_VAL(pid)		((pid) & GENMASK_ULL(31, 0))
#define I3C_PID_PART_ID(pid)		(((pid) & GENMASK_ULL(31, 16)) >> 16)
#define I3C_PID_INSTANCE_ID(pid)	(((pid) & GENMASK_ULL(15, 12)) >> 12)
#define I3C_PID_EXTRA_INFO(pid)		((pid) & GENMASK_ULL(11, 0))

#define I3C_BCR_DEVICE_ROLE(bcr)	((bcr) & GENMASK(7, 6))
#define I3C_BCR_I3C_SLAVE		(0 << 6)
#define I3C_BCR_I3C_MASTER		(1 << 6)
#define I3C_BCR_HDR_CAP			BIT(5)
#define I3C_BCR_BRIDGE			BIT(4)
#define I3C_BCR_OFFLINE_CAP		BIT(3)
#define I3C_BCR_IBI_PAYLOAD		BIT(2)
#define I3C_BCR_IBI_REQ_CAP		BIT(1)
#define I3C_BCR_MAX_DATA_SPEED_LIM	BIT(0)

/**
 * struct i3c_device_info - I3C device information
 *
 * @pid: Provisional ID
 * @bcr: Bus Characteristic Register
 * @dcr: Device Characteristic Register
 * @static_addr: static/I2C address
 * @dyn_addr: dynamic address
 * @hdr_cap: supported HDR modes
 * @max_read_ds: max read speed information
 * @max_write_ds: max write speed information
 * @max_ibi_len: max IBI payload length
 * @max_read_turnaround: max read turn-around time in micro-seconds
 * @max_read_len: max private SDR read length in bytes
 * @max_write_len: max private SDR write length in bytes
 *
 * These are all basic information that should be advertised by an I3C device.
 * Some of them are optional depending on the device type and device
 * capabilities.
 * For each I3C slave attached to a master with
 * i3c_master_add_i3c_dev_locked(), the core will send the relevant CCC command
 * to retrieve these data.
 */
struct i3c_device_info {
	u64 pid;
	u8 bcr;
	u8 dcr;
	u8 static_addr;
	u8 dyn_addr;
	u8 hdr_cap;
	u8 max_read_ds;
	u8 max_write_ds;
	u8 max_ibi_len;
	u32 max_read_turnaround;
	u16 max_read_len;
	u16 max_write_len;
};

/*
 * I3C device internals are kept hidden from I3C device users. It's just
 * simpler to refactor things when everything goes through getter/setters, and
 * I3C device drivers should not have to worry about internal representation
 * anyway.
 */
struct i3c_device;

/* These macros should be used to i3c_device_id entries. */
#define I3C_MATCH_MANUF_AND_PART (I3C_MATCH_MANUF | I3C_MATCH_PART)

#define I3C_DEVICE(manuf, part)						\
	{								\
		.match_flags = I3C_MATCH_MANUF_AND_PART,		\
		.manuf_id = manuf,					\
		.part_id = part,					\
	}

#define I3C_DEVICE_EXTRA_INFO(manuf, part, info)			\
	{								\
		.match_flags = I3C_MATCH_MANUF_AND_PART |		\
			       I3C_MATCH_EXTRA_INFO,			\
		.manuf_id = manuf,					\
		.part_id = part,					\
		.extra_info = info,					\
	}

#define I3C_CLASS(__dcr)						\
	{								\
		.match_flags = I3C_MATCH_DCR,				\
		.dcr = __dcr,						\
	}

/**
 * struct i3c_driver - I3C device driver
 * @driver: inherit from device_driver
 * @probe: I3C device probe method
 * @remove: I3C device remove method
 * @id_table: I3C device match table. Will be used by the framework to decide
 *	      which device to bind to this driver
 */
struct i3c_driver {
	struct device_driver driver;
	int (*probe)(struct i3c_device *dev);
	int (*remove)(struct i3c_device *dev);
	const struct i3c_device_id *id_table;
};

static inline struct i3c_driver *drv_to_i3cdrv(struct device_driver *drv)
{
	return container_of(drv, struct i3c_driver, driver);
}

int i3c_driver_register_with_owner(struct i3c_driver *drv,
				   struct module *owner);
void i3c_driver_unregister(struct i3c_driver *drv);

#define i3c_driver_register(drv)	\
	i3c_driver_register_with_owner(drv, THIS_MODULE)

#define module_i3c_driver(i3cdrv) \
	module_driver(i3cdrv, i3c_driver_register, i3c_driver_unregister)

int i3c_device_do_priv_xfers(struct i3c_device *dev,
			     struct i3c_priv_xfer *xfers,
			     int nxfers);
int i3c_device_send_hdr_cmds(struct i3c_device *dev,
			     struct i3c_hdr_cmd *cmds,
			     int ncmds);

void i3c_device_get_info(struct i3c_device *dev, struct i3c_device_info *info);

#endif /* I3C_DEV_H */
