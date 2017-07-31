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

#ifndef I3C_MASTER_H
#define I3C_MASTER_H

#include <linux/i2c.h>
#include <linux/i3c/ccc.h>
#include <linux/i3c/device.h>

#define I3C_HOT_JOIN_ADDR		0x2
#define I3C_BROADCAST_ADDR		0x7e
#define I3C_MAX_ADDR			GENMASK(6, 0)

struct i3c_master_controller;
struct i3c_bus;

/**
 * struct i3c_i2c_dev - I3C/I2C common information
 *
 * @node: node element used to insert the device into the I2C or I3C device
 *	  list
 * @bus: I3C bus this device is connected to
 * @master: I3C master that instantiated this device. Will be used to send
 *	    I2C/I3C frames on the bus
 * @master_priv: master private data assigned to the device. Can be used to
 *		 add master specific information
 *
 * This structure is describing common I3C/I2C dev information.
 */
struct i3c_i2c_dev {
	struct list_head node;
	struct i3c_bus *bus;
	struct i3c_master_controller *master;
	void *master_priv;
};

#define I3C_LVR_I2C_INDEX_MASK		GENMASK(7, 5)
#define I3C_LVR_I2C_INDEX(x)		((x) << 5)
#define I3C_LVR_I2C_FM_MODE		BIT(4)

#define I2C_MAX_ADDR			GENMASK(9, 0)

/**
 * struct i2c_device - I2C device object
 *
 * @common: inherit common I3C/I2C description
 * @info: I2C board info used to instantiate the I2C device. If you are
 *	  using DT to describe your hardware, this will be filled for you.
 * @client: I2C client object created by the I2C framework. This will only
 *	    be valid after i3c_master_register() returns.
 * @lvr: Legacy Virtual Register value as described in the I3C specification
 *
 * I2C device object. Note that the real I2C device is represented by
 * i2c_device->client, but we need extra information to handle the device when
 * it's connected to an I3C bus, hence the &struct i2c_device wrapper.
 *
 * The I2C framework is not impacted by this new representation.
 */
struct i2c_device {
	struct i3c_i2c_dev common;
	struct i2c_board_info info;
	struct i2c_client *client;
	u8 lvr;
};

/**
 * struct i3c_device - I3C device object
 *
 * @common: inherit common I3C/I2C description
 * @dev: device object to register the I3C dev to the device model
 * @info: I3C device information. Will be automatically filled when you create
 *	  your device with i3c_master_add_i3c_dev_locked().
 *
 * I3C device object. Every I3C devs on the I3C bus are represented, including
 * I3C masters. For each of them, we have an instance of &struct i3c_device.
 */
struct i3c_device {
	struct i3c_i2c_dev common;
	struct device dev;
	struct i3c_device_info info;
};

/*
 * The I3C specification says the maximum number of devices connected on the
 * bus is 11, but this number depends on external parameters like trace length,
 * capacitive load per Device, and the types of Devices present on the Bus.
 * I3C master can also have limitations, so this number is just here as a
 * reference and should be adjusted on a per-controller/per-board basis.
 */
#define I3C_BUS_MAX_DEVS		11

#define I3C_BUS_MAX_I3C_SCL_RATE	12900000
#define I3C_BUS_TYP_I3C_SCL_RATE	12500000
#define I3C_BUS_I2C_FM_PLUS_SCL_RATE	1000000
#define I3C_BUS_I2C_FM_SCL_RATE		400000
#define I3C_BUS_TLOW_OD_MIN_NS		200

/**
 * enum i3c_bus_mode - I3C bus mode
 *
 * @I3C_BUS_MODE_PURE: only I3C devices are connected to the bus. No limitation
 *		       expected
 * @I3C_BUS_MODE_MIXED_FAST: I2C devices with 50ns spike filter are present on
 *			     the bus. The only impact in this mode is that the
 *			     high SCL pulse has to stay below 50ns to trick I2C
 *			     devices when transmitting I3C frames
 * @I3C_BUS_MODE_MIXED_SLOW: I2C devices without 50ns spike filter are present
 *			     on the bus
 */
enum i3c_bus_mode {
	I3C_BUS_MODE_PURE,
	I3C_BUS_MODE_MIXED_FAST,
	I3C_BUS_MODE_MIXED_SLOW,
};

/**
 * enum i3c_addr_slot_status - I3C address slot status
 *
 * @I3C_ADDR_SLOT_FREE: address is free
 * @I3C_ADDR_SLOT_RSVD: address is reserved
 * @I3C_ADDR_SLOT_I2C_DEV: address is assigned to an I2C device
 * @I3C_ADDR_SLOT_I3C_DEV: address is assigned to an I3C device
 * @I3C_ADDR_SLOT_STATUS_MASK: address slot mask
 *
 * On an I3C bus, addresses are assigned dynamically, and we need to know which
 * addresses are free to use and which ones are already assigned.
 *
 * Addresses marked as reserved are those reserved by the I3C protocol
 * (broadcast address, ...).
 */
enum i3c_addr_slot_status {
	I3C_ADDR_SLOT_FREE,
	I3C_ADDR_SLOT_RSVD,
	I3C_ADDR_SLOT_I2C_DEV,
	I3C_ADDR_SLOT_I3C_DEV,
	I3C_ADDR_SLOT_STATUS_MASK = 3,
};

/**
 * struct i3c_bus - I3C bus object
 *
 * @dev: device to be registered to the device-model
 * @cur_master: I3C master currently driving the bus. Since I3C is multi-master
 *		this can change over the time. Will be used to let a master
 *		know whether it needs to request bus ownership before sending
 *		a frame or not
 * @addrslots: a bitmap with 2-bits per-slot to encode the address status and
 *	       ease the DAA (Dynamic Address Assignment) procedure (see
 *	       &enum i3c_addr_slot_status)
 * @mode: bus mode (see &enum i3c_bus_mode)
 * @scl_rate: SCL signal rate for I3C and I2C mode
 * @devs: 2 lists containing all I3C/I2C devices connected to the bus
 * @lock: read/write lock on the bus. This is needed to protect against
 *	  operations that have an impact on the whole bus and the devices
 *	  connected to it. For example, when asking slaves to drop their
 *	  dynamic address (RSTDAA CCC), we need to make sure no one is trying
 *	  to send I3C frames to these devices.
 *	  Note that this lock does not protect against concurrency between
 *	  devices: several drivers can send different I3C/I2C frames through
 *	  the same master in parallel. This is the responsibility of the
 *	  master to guarantee that frames are actually sent sequentially and
 *	  not interlaced.
 *
 * The I3C bus is represented with its own object and not implicitly described
 * by the I3C master to cope with the multi-master functionality, where one bus
 * can be shared amongst several masters, each of them requesting bus ownership
 * when they need to.
 */
struct i3c_bus {
	struct device dev;
	struct i3c_device *cur_master;
	int id;
	unsigned long addrslots[((I2C_MAX_ADDR + 1) * 2) / BITS_PER_LONG];
	enum i3c_bus_mode mode;
	struct {
		unsigned long i3c;
		unsigned long i2c;
	} scl_rate;
	struct {
		struct list_head i3c;
		struct list_head i2c;
	} devs;
	struct rw_semaphore lock;
};

static inline struct i3c_device *dev_to_i3cdev(struct device *dev)
{
	return container_of(dev, struct i3c_device, dev);
}

struct i3c_master_controller;

/**
 * struct i3c_master_controller_ops - I3C master methods
 *
 * @bus_init: hook responsible for the I3C bus initialization. This
 *	      initialization should follow the steps described in the I3C
 *	      specification. This hook is called with the bus lock held in
 *	      write mode, which means all _locked() helpers can safely be
 *	      called from there.
 * @bus_cleanup: cleanup everything done in
 *		 &i3c_master_controller_ops->bus_init(). This function is
 *		 optional and should only be implemented if
 *		 &i3c_master_controller_ops->bus_init() attached private data
 *		 to I3C/I2C devices. This hook is called with the bus lock
 *		 held in write mode, which means all _locked() helpers can
 *		 safely be called from there.
 * @supports_ccc_cmd: should return true if the CCC command is supported, false
 *		      otherwise
 * @send_ccc_cmd: send a CCC command
 * @send_hdr_cmds: send one or several HDR commands. If there is more than one
 *		   command, they should ideally be sent in the same HDR
 *		   transaction
 * @priv_xfers: do one or several private I3C SDR transfers
 * @i2c_xfers: do one or several I2C transfers
 *
 * One of the most important hooks in these ops is
 * &i3c_master_controller_ops->bus_init(). Here is a non-exhaustive list of
 * things that should be done in &i3c_master_controller_ops->bus_init():
 *
 * 1) call i3c_master_set_info() with all information describing the master
 * 2) ask all slaves to drop their dynamic address by sending the RSTDAA CCC
 *    with i3c_master_rstdaa_locked()
 * 3) ask all slaves to disable IBIs using i3c_master_disec_locked()
 * 4) start a DDA procedure by sending the ENTDAA CCC with
 *    i3c_master_entdaa_locked(), or using the internal DAA logic provided by
 *    your controller
 * 5) assign a dynamic address to each I3C device discovered during DAA and
 *    for each of them, call i3c_master_add_i3c_dev_locked()
 * 6) propagate device table to secondary masters by calling
 *    i3c_master_defslvs_locked()
 *
 * Note that these steps do not include all controller specific initialization.
 */
struct i3c_master_controller_ops {
	int (*bus_init)(struct i3c_master_controller *master);
	void (*bus_cleanup)(struct i3c_master_controller *master);
	bool (*supports_ccc_cmd)(struct i3c_master_controller *master,
				 const struct i3c_ccc_cmd *cmd);
	int (*send_ccc_cmd)(struct i3c_master_controller *master,
			    struct i3c_ccc_cmd *cmd);
	int (*send_hdr_cmds)(struct i3c_master_controller *master,
			     const struct i3c_hdr_cmd *cmds,
			     int ncmds);
	int (*priv_xfers)(struct i3c_master_controller *master,
			  const struct i3c_priv_xfer *xfers,
			  int nxfers);
	int (*i2c_xfers)(struct i3c_master_controller *master,
			 const struct i2c_msg *xfers, int nxfers);
};

/**
 * struct i3c_master_controller - I3C master controller object
 *
 * @parent: parent device that instantiated this master
 * @base: inherit from &struct i3c_device. A master is just a I3C device that
 *	  has to be represented on the bus
 * @i2c: I2C adapter used for backward compatibility. This adapter is
 *	 registered to the I2C subsystem to be as transparent as possible to
 *	 existing I2C drivers
 * @ops: master operations. See &struct i3c_master_controller_ops
 * @secondary: true if the master is a secondary master
 * @bus: I3C bus object created by this master
 *
 * A &struct i3c_master_controller has to be registered to the I3C subsystem
 * through i3c_master_register(). None of &struct i3c_master_controller fields
 * should be set manually, just pass appropriate values to
 * i3c_master_register().
 */
struct i3c_master_controller {
	struct device *parent;
	struct i3c_device base;
	struct i2c_adapter i2c;
	const struct i3c_master_controller_ops *ops;
	bool secondary;
	struct i3c_bus *bus;
};

/**
 * i3c_bus_for_each_i2cdev() - iterate over all I2C devices present on the bus
 *
 * @bus: the I3C bus
 * @i2cdev: an I2C device updated to point to the current device at each loop
 *	    iteration
 *
 * Iterate over all I2C devs present on the bus.
 */
#define i3c_bus_for_each_i2cdev(bus, i2cdev)				\
	list_for_each_entry(i2cdev, &(bus)->devs.i2c, common.node)

/**
 * i3c_bus_for_each_i3cdev() - iterate over all I3C devices present on the bus
 *
 * @bus: the I3C bus
 * @i3cdev: an I3C device updated to point to the current device at each loop
 *	    iteration
 *
 * Iterate over all I3C devs present on the bus.
 */
#define i3c_bus_for_each_i3cdev(bus, i3cdev)				\
	list_for_each_entry(i3cdev, &(bus)->devs.i3c, common.node)

int i3c_master_send_hdr_cmds(struct i3c_master_controller *master,
			     const struct i3c_hdr_cmd *cmds,
			     int ncmds);
int i3c_master_do_priv_xfers(struct i3c_master_controller *master,
			     const struct i3c_priv_xfer *xfers,
			     int nxfers);
int i3c_master_do_i2c_xfers(struct i3c_master_controller *master,
			    const struct i2c_msg *xfers,
			    int nxfers);

int i3c_master_disec_locked(struct i3c_master_controller *master, u8 addr,
			    const struct i3c_ccc_events *evts);
int i3c_master_rstdaa_locked(struct i3c_master_controller *master, u8 addr);
int i3c_master_entdaa_locked(struct i3c_master_controller *master);
int i3c_master_defslvs_locked(struct i3c_master_controller *master);

int i3c_master_get_free_addr(struct i3c_master_controller *master,
			     u8 start_addr);

struct i3c_device *
i3c_master_add_i3c_dev_locked(struct i3c_master_controller *master, u8 addr);

int i3c_master_set_info(struct i3c_master_controller *master,
			const struct i3c_device_info *info);

int i3c_master_register(struct i3c_master_controller *master,
			struct device *parent,
			const struct i3c_master_controller_ops *ops,
			bool secondary);
int i3c_master_unregister(struct i3c_master_controller *master);

/**
 * i3c_device_get_master_data() - get master private data attached to an I3C
 *				  device
 *
 * @dev: the I3C dev to attach private data to
 *
 * Return: the private data previously attached with
 *	   i3c_device_set_master_data() or NULL if no data has been attached
 *	   to the device.
 */
static inline void *i3c_device_get_master_data(const struct i3c_device *dev)
{
	return dev->common.master_priv;
}

/**
 * i3c_device_set_master_data() - attach master private data to an I3C device
 *
 * @dev: the I3C dev to attach private data to
 * @data: private data
 *
 * This functions allows a master controller to attach per-device private data
 * which can then be retrieved with i3c_device_get_master_data().
 *
 * Attaching private data to a device is usually done just after calling
 * i3c_master_add_i3c_dev_locked().
 */
static inline void i3c_device_set_master_data(struct i3c_device *dev,
					      void *data)
{
	dev->common.master_priv = data;
}

/**
 * i2c_device_get_master_data() - get master private data attached to an I2C
 *				  device
 *
 * @dev: the I2C dev to attach private data to
 *
 * Return: the private data previously attached with
 *	   i2c_device_set_master_data() or NULL if no data has been attached
 *	   to the device.
 */
static inline void *i2c_device_get_master_data(const struct i2c_device *dev)
{
	return dev->common.master_priv;
}

/**
 * i2c_device_set_master_data() - attach master private data to an I2C device
 *
 * @dev: the I2C dev to attach private data to
 * @data: private data
 *
 * This functions allows a master controller to attach per-device private data
 * which can then be retrieved with i2c_device_get_master_data().
 *
 * Attaching private data to a device is usually done during
 * &master_controller_ops->bus_init(), by iterating over all I2C devices
 * instantiated by the core (using i3c_bus_for_each_i2cdev()).
 */
static inline void i2c_device_set_master_data(struct i2c_device *dev,
					      void *data)
{
	dev->common.master_priv = data;
}

/**
 * i3c_device_get_master() - get master used to communicate with a device
 *
 * @dev: I3C dev
 *
 * Return: the master controller driving @dev
 */
static inline struct i3c_master_controller *
i3c_device_get_master(struct i3c_device *dev)
{
	return dev->common.master;
}

/**
 * i3c_master_get_bus() - get the bus attached to a master
 *
 * @master: master object
 *
 * Return: the I3C bus @master is connected to
 */
static inline struct i3c_bus *
i3c_master_get_bus(struct i3c_master_controller *master)
{
	return master->bus;
}

/**
 * i3c_device_get_bus() - get the bus attached to a device
 *
 * @dev: an I3C device
 *
 * Return: the I3C bus @dev is connected to
 */
static inline struct i3c_bus *i3c_device_get_bus(struct i3c_device *dev)
{
	return i3c_master_get_bus(i3c_device_get_master(dev));
}

#endif /* I3C_MASTER_H */
