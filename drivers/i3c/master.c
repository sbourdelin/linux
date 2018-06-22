// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "internals.h"

static struct i3c_master_controller *
i2c_adapter_to_i3c_master(struct i2c_adapter *adap)
{
	return container_of(adap, struct i3c_master_controller, i2c);
}

static struct i2c_adapter *
i3c_master_to_i2c_adapter(struct i3c_master_controller *master)
{
	return &master->i2c;
}

static struct i2c_device *
i3c_master_alloc_i2c_dev(struct i3c_master_controller *master,
			 const struct i2c_board_info *info, u8 lvr)
{
	struct i2c_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev->common.bus = master->bus;
	dev->info = *info;
	dev->lvr = lvr;
	dev->info.of_node = of_node_get(info->of_node);
	i3c_bus_set_addr_slot_status(master->bus, info->addr,
				     I3C_ADDR_SLOT_I2C_DEV);

	return dev;
}

static int i3c_master_send_ccc_cmd_locked(struct i3c_master_controller *master,
					  struct i3c_ccc_cmd *cmd)
{
	if (!cmd || !master)
		return -EINVAL;

	if (WARN_ON(master->init_done &&
		    !rwsem_is_locked(&master->bus->lock)))
		return -EINVAL;

	if (!master->ops->send_ccc_cmd)
		return -ENOTSUPP;

	if ((cmd->id & I3C_CCC_DIRECT) && (!cmd->dests || !cmd->ndests))
		return -EINVAL;

	if (master->ops->supports_ccc_cmd &&
	    !master->ops->supports_ccc_cmd(master, cmd))
		return -ENOTSUPP;

	return master->ops->send_ccc_cmd(master, cmd);
}

static struct i2c_device *
i3c_master_find_i2c_dev_by_addr(const struct i3c_master_controller *master,
				u16 addr)
{
	struct i2c_device *dev;

	i3c_bus_for_each_i2cdev(master->bus, dev) {
		if (dev->client->addr == addr)
			return dev;
	}

	return NULL;
}

/**
 * i3c_master_get_free_addr() - get a free address on the bus
 * @master: I3C master object
 * @start_addr: where to start searching
 *
 * This function must be called with the bus lock held in write mode.
 *
 * Return: the first free address starting at @start_addr (included) or -ENOMEM
 * if there's no more address available.
 */
int i3c_master_get_free_addr(struct i3c_master_controller *master,
			     u8 start_addr)
{
	return i3c_bus_get_free_addr(master->bus, start_addr);
}
EXPORT_SYMBOL_GPL(i3c_master_get_free_addr);

static void i3c_device_release(struct device *dev)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);

	if (i3cdev->info.static_addr)
		i3c_bus_set_addr_slot_status(i3cdev->common.bus,
					     i3cdev->info.static_addr,
					     I3C_ADDR_SLOT_FREE);

	if (i3cdev->info.dyn_addr)
		i3c_bus_set_addr_slot_status(i3cdev->common.bus,
					     i3cdev->info.dyn_addr,
					     I3C_ADDR_SLOT_FREE);

	of_node_put(dev->of_node);
	kfree(i3cdev);
}

static struct i3c_device *
i3c_master_alloc_i3c_dev(struct i3c_master_controller *master,
			 const struct i3c_device_info *info,
			 const struct device_type *devtype)
{
	struct i3c_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	dev->common.bus = master->bus;
	dev->dev.parent = &master->bus->dev;
	dev->dev.type = devtype;
	dev->dev.bus = &i3c_bus_type;
	dev->dev.release = i3c_device_release;
	dev->info = *info;
	mutex_init(&dev->ibi_lock);
	dev_set_name(&dev->dev, "%d-%llx", master->bus->id, info->pid);

	device_initialize(&dev->dev);

	if (info->static_addr)
		i3c_bus_set_addr_slot_status(master->bus, info->static_addr,
					     I3C_ADDR_SLOT_I3C_DEV);

	if (info->dyn_addr)
		i3c_bus_set_addr_slot_status(master->bus, info->dyn_addr,
					     I3C_ADDR_SLOT_I3C_DEV);

	return dev;
}

/**
 * i3c_master_set_info() - set master device information
 * @master: master used to send frames on the bus
 * @info: I3C device information
 *
 * Set master device info. This should be called from
 * &i3c_master_controller_ops->bus_init().
 *
 * Not all &i3c_device_info fields are meaningful for a master device.
 * Here is a list of fields that should be properly filled:
 *
 * - &i3c_device_info->dyn_addr
 * - &i3c_device_info->bcr
 * - &i3c_device_info->dcr
 * - &i3c_device_info->pid
 * - &i3c_device_info->hdr_cap if %I3C_BCR_HDR_CAP bit is set in
 *   &i3c_device_info->bcr
 *
 * This function must be called with the bus lock held in maintenance mode.
 *
 * Return: 0 if @info contains valid information (not every piece of
 * information can be checked, but we can at least make sure @info->dyn_addr
 * and @info->bcr are correct), -EINVAL otherwise.
 */
int i3c_master_set_info(struct i3c_master_controller *master,
			const struct i3c_device_info *info)
{
	struct i3c_device *i3cdev;

	if (!i3c_bus_dev_addr_is_avail(master->bus, info->dyn_addr))
		return -EINVAL;

	if (I3C_BCR_DEVICE_ROLE(info->bcr) == I3C_BCR_I3C_MASTER &&
	    master->secondary)
		return -EINVAL;

	if (master->this)
		return -EINVAL;

	i3cdev = i3c_master_alloc_i3c_dev(master, info,	&i3c_master_type);
	if (IS_ERR(i3cdev))
		return PTR_ERR(i3cdev);

	master->this = i3cdev;
	master->bus->cur_master = master->this;
	i3cdev->common.bus = master->bus;
	i3cdev->common.master = master;
	list_add_tail(&i3cdev->common.node, &master->bus->devs.i3c);

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_master_set_info);

static int i3c_master_rstdaa_locked(struct i3c_master_controller *master,
				    u8 addr)
{
	struct i3c_ccc_cmd_dest dest = { };
	struct i3c_ccc_cmd cmd = { };
	enum i3c_addr_slot_status addrstat;
	int ret;

	if (!master)
		return -EINVAL;

	addrstat = i3c_bus_get_addr_slot_status(master->bus, addr);
	if (addr != I3C_BROADCAST_ADDR && addrstat != I3C_ADDR_SLOT_I3C_DEV)
		return -EINVAL;

	dest.addr = addr;
	cmd.dests = &dest;
	cmd.ndests = 1;
	cmd.rnw = false;
	cmd.id = I3C_CCC_RSTDAA(addr == I3C_BROADCAST_ADDR);

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	return 0;
}

/**
 * i3c_master_entdaa_locked() - start a DAA (Dynamic Address Assignment)
 *				procedure
 * @master: master used to send frames on the bus
 *
 * Send a ENTDAA CCC command to start a DAA procedure.
 *
 * Note that this function only sends the ENTDAA CCC command, all the logic
 * behind dynamic address assignment has to be handled in the I3C master
 * driver.
 *
 * This function must be called with the bus lock held in write mode.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_entdaa_locked(struct i3c_master_controller *master)
{
	struct i3c_ccc_cmd_dest dest = { };
	struct i3c_ccc_cmd cmd = { };
	int ret;

	dest.addr = I3C_BROADCAST_ADDR;
	cmd.dests = &dest;
	cmd.ndests = 1;
	cmd.rnw = false;
	cmd.id = I3C_CCC_ENTDAA;

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_master_entdaa_locked);

/**
 * i3c_master_disec_locked() - send a DISEC CCC command
 * @master: master used to send frames on the bus
 * @addr: a valid I3C slave address or %I3C_BROADCAST_ADDR
 * @evts: events to disable
 *
 * Send a DISEC CCC command to disable some or all events coming from a
 * specific slave, or all devices if @addr is %I3C_BROADCAST_ADDR.
 *
 * This function must be called with the bus lock held in write mode.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_disec_locked(struct i3c_master_controller *master, u8 addr,
			    u8 evts)
{
	struct i3c_ccc_events events = {
		.events = evts,
	};
	struct i3c_ccc_cmd_dest dest = {
		.addr = addr,
		.payload.len = sizeof(events),
		.payload.data = &events,
	};
	struct i3c_ccc_cmd cmd = {
		.id = I3C_CCC_DISEC(addr == I3C_BROADCAST_ADDR),
		.dests = &dest,
		.ndests = 1,
	};

	return i3c_master_send_ccc_cmd_locked(master, &cmd);
}
EXPORT_SYMBOL_GPL(i3c_master_disec_locked);

/**
 * i3c_master_enec_locked() - send an ENEC CCC command
 * @master: master used to send frames on the bus
 * @addr: a valid I3C slave address or %I3C_BROADCAST_ADDR
 * @evts: events to disable
 *
 * Sends an ENEC CCC command to enable some or all events coming from a
 * specific slave, or all devices if @addr is %I3C_BROADCAST_ADDR.
 *
 * This function must be called with the bus lock held in write mode.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_enec_locked(struct i3c_master_controller *master, u8 addr,
			   u8 evts)
{
	struct i3c_ccc_events events = {
		.events = evts,
	};
	struct i3c_ccc_cmd_dest dest = {
		.addr = addr,
		.payload.len = sizeof(events),
		.payload.data = &events,
	};
	struct i3c_ccc_cmd cmd = {
		.id = I3C_CCC_ENEC(addr == I3C_BROADCAST_ADDR),
		.dests = &dest,
		.ndests = 1,
	};

	return i3c_master_send_ccc_cmd_locked(master, &cmd);
}
EXPORT_SYMBOL_GPL(i3c_master_enec_locked);

/**
 * i3c_master_defslvs_locked() - send a DEFSLVS CCC command
 * @master: master used to send frames on the bus
 *
 * Send a DEFSLVS CCC command containing all the devices known to the @master.
 * This is useful when you have secondary masters on the bus to propagate
 * device information.
 *
 * This should be called after all I3C devices have been discovered (in other
 * words, after the DAA procedure has finished) and instantiated in
 * i3c_master_controller_ops->bus_init().
 * It should also be called if a master ACKed an Hot-Join request and assigned
 * a dynamic address to the device joining the bus.
 *
 * This function must be called with the bus lock held in write mode.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_defslvs_locked(struct i3c_master_controller *master)
{
	struct i3c_ccc_cmd_dest dest = {
		.addr = I3C_BROADCAST_ADDR,
	};
	struct i3c_ccc_cmd cmd = {
		.id = I3C_CCC_DEFSLVS,
		.dests = &dest,
		.ndests = 1,
	};
	struct i3c_ccc_defslvs *defslvs;
	struct i3c_ccc_dev_desc *desc;
	struct i3c_device *i3cdev;
	struct i2c_device *i2cdev;
	struct i3c_bus *bus;
	bool send = false;
	int ndevs = 0, ret;

	if (!master)
		return -EINVAL;

	bus = i3c_master_get_bus(master);
	i3c_bus_for_each_i3cdev(bus, i3cdev) {
		ndevs++;
		if (I3C_BCR_DEVICE_ROLE(i3cdev->info.bcr) == I3C_BCR_I3C_MASTER)
			send = true;
	}

	/* No other master on the bus, skip DEFSLVS. */
	if (!send)
		return 0;

	i3c_bus_for_each_i2cdev(bus, i2cdev)
		ndevs++;

	dest.payload.len = sizeof(*defslvs) +
			   ((ndevs - 1) * sizeof(struct i3c_ccc_dev_desc));
	defslvs = kzalloc(dest.payload.len, GFP_KERNEL);
	if (!defslvs)
		return -ENOMEM;

	dest.payload.data = defslvs;

	defslvs->count = ndevs;
	defslvs->master.bcr = master->this->info.bcr;
	defslvs->master.dcr = master->this->info.dcr;
	defslvs->master.dyn_addr = master->this->info.dyn_addr << 1;
	defslvs->master.static_addr = I3C_BROADCAST_ADDR << 1;

	desc = defslvs->slaves;
	i3c_bus_for_each_i2cdev(bus, i2cdev) {
		desc->lvr = i2cdev->lvr;
		desc->static_addr = i2cdev->info.addr << 1;
		desc++;
	}

	i3c_bus_for_each_i3cdev(bus, i3cdev) {
		/* Skip the I3C dev representing this master. */
		if (i3cdev == master->this)
			continue;

		desc->bcr = i3cdev->info.bcr;
		desc->dcr = i3cdev->info.dcr;
		desc->dyn_addr = i3cdev->info.dyn_addr << 1;
		desc->static_addr = i3cdev->info.static_addr << 1;
		desc++;
	}

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	kfree(defslvs);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_master_defslvs_locked);

static int i3c_master_setdasa_locked(struct i3c_master_controller *master,
				     u8 static_addr, u8 dyn_addr)
{
	struct i3c_ccc_setda setda = {
		.addr = dyn_addr << 1,
	};
	struct i3c_ccc_cmd_dest dest = {
		.addr = static_addr,
		.payload.len = sizeof(setda),
		.payload.data = &setda,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = false,
		.id = I3C_CCC_SETDASA,
		.dests = &dest,
		.ndests = 1,
	};

	if (!dyn_addr || !static_addr)
		return -EINVAL;

	return i3c_master_send_ccc_cmd_locked(master, &cmd);
}

static int i3c_master_setnewda_locked(struct i3c_master_controller *master,
				      u8 oldaddr, u8 newaddr)
{
	struct i3c_ccc_setda setda = {
		.addr = newaddr << 1,
	};
	struct i3c_ccc_cmd_dest dest = {
		.addr = oldaddr,
		.payload.len = sizeof(setda),
		.payload.data = &setda,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = false,
		.id = I3C_CCC_SETNEWDA,
		.dests = &dest,
		.ndests = 1,
	};

	if (!oldaddr || !newaddr)
		return -EINVAL;

	return i3c_master_send_ccc_cmd_locked(master, &cmd);
}

static int i3c_master_getmrl_locked(struct i3c_master_controller *master,
				    struct i3c_device_info *info)
{
	struct i3c_ccc_mrl mrl;
	struct i3c_ccc_cmd_dest dest = {
		.addr = info->dyn_addr,
		.payload.len = sizeof(mrl),
		.payload.data = &mrl,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = true,
		.id = I3C_CCC_GETMRL,
		.dests = &dest,
		.ndests = 1,
	};
	int ret;

	/*
	 * When the device does not have IBI payload GETMRL only returns 2
	 * bytes of data.
	 */
	if (!(info->bcr & I3C_BCR_IBI_PAYLOAD))
		dest.payload.len -= 1;

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	if (dest.payload.len != sizeof(mrl))
		return -EIO;

	info->max_read_len = be16_to_cpu(mrl.read_len);

	if (info->bcr & I3C_BCR_IBI_PAYLOAD)
		info->max_ibi_len = mrl.ibi_len;

	return 0;
}

static int i3c_master_getmwl_locked(struct i3c_master_controller *master,
				    struct i3c_device_info *info)
{
	struct i3c_ccc_mwl mwl;
	struct i3c_ccc_cmd_dest dest = {
		.addr = info->dyn_addr,
		.payload.len = sizeof(mwl),
		.payload.data = &mwl,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = true,
		.id = I3C_CCC_GETMWL,
		.dests = &dest,
		.ndests = 1,
	};
	int ret;

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	if (dest.payload.len != sizeof(mwl))
		return -EIO;

	info->max_write_len = be16_to_cpu(mwl.len);

	return 0;
}

static int i3c_master_getmxds_locked(struct i3c_master_controller *master,
				     struct i3c_device_info *info)
{
	struct i3c_ccc_getmxds getmaxds;
	struct i3c_ccc_cmd_dest dest = {
		.addr = info->dyn_addr,
		.payload.len = sizeof(getmaxds),
		.payload.data = &getmaxds,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = true,
		.id = I3C_CCC_GETMXDS,
		.dests = &dest,
		.ndests = 1,
	};
	int ret;

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	if (dest.payload.len != 2 && dest.payload.len != 5)
		return -EIO;

	info->max_read_ds = getmaxds.maxrd;
	info->max_read_ds = getmaxds.maxwr;
	if (dest.payload.len == 5)
		info->max_read_turnaround = getmaxds.maxrdturn[0] |
					    ((u32)getmaxds.maxrdturn[1] << 8) |
					    ((u32)getmaxds.maxrdturn[2] << 16);

	return 0;
}

static int i3c_master_gethdrcap_locked(struct i3c_master_controller *master,
				       struct i3c_device_info *info)
{
	struct i3c_ccc_gethdrcap gethdrcap;
	struct i3c_ccc_cmd_dest dest = {
		.addr = info->dyn_addr,
		.payload.len = sizeof(gethdrcap),
		.payload.data = &gethdrcap,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = true,
		.id = I3C_CCC_GETHDRCAP,
		.dests = &dest,
		.ndests = 1,
	};
	int ret;

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	if (dest.payload.len != 1)
		return -EIO;

	info->hdr_cap = gethdrcap.modes;

	return 0;
}

static int i3c_master_getpid_locked(struct i3c_master_controller *master,
				    struct i3c_device_info *info)
{
	struct i3c_ccc_getpid getpid;
	struct i3c_ccc_cmd_dest dest = {
		.addr = info->dyn_addr,
		.payload.len = sizeof(struct i3c_ccc_getpid),
		.payload.data = &getpid,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = true,
		.id = I3C_CCC_GETPID,
		.dests = &dest,
		.ndests = 1,
	};
	int ret, i;

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	info->pid = 0;
	for (i = 0; i < sizeof(getpid.pid); i++) {
		int sft = (sizeof(getpid.pid) - i - 1) * 8;

		info->pid |= (u64)getpid.pid[i] << sft;
	}

	return 0;
}

static int i3c_master_getbcr_locked(struct i3c_master_controller *master,
				    struct i3c_device_info *info)
{
	struct i3c_ccc_getbcr getbcr;
	struct i3c_ccc_cmd_dest dest = {
		.addr = info->dyn_addr,
		.payload.len = sizeof(struct i3c_ccc_getbcr),
		.payload.data = &getbcr,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = true,
		.id = I3C_CCC_GETBCR,
		.dests = &dest,
		.ndests = 1,
	};
	int ret;

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	info->bcr = getbcr.bcr;

	return 0;
}

static int i3c_master_getdcr_locked(struct i3c_master_controller *master,
				    struct i3c_device_info *info)
{
	struct i3c_ccc_getdcr getdcr;
	struct i3c_ccc_cmd_dest dest = {
		.addr = info->dyn_addr,
		.payload.len = sizeof(struct i3c_ccc_getdcr),
		.payload.data = &getdcr,
	};
	struct i3c_ccc_cmd cmd = {
		.rnw = true,
		.id = I3C_CCC_GETDCR,
		.dests = &dest,
		.ndests = 1,
	};
	int ret;

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	if (ret)
		return ret;

	info->dcr = getdcr.dcr;

	return 0;
}

static int i3c_master_retrieve_dev_info(struct i3c_master_controller *master,
					struct i3c_device_info *info, u8 addr)
{
	enum i3c_addr_slot_status slot_status;
	int ret;

	if (!master || !info)
		return -EINVAL;

	memset(info, 0, sizeof(*info));
	info->dyn_addr = addr;

	slot_status = i3c_bus_get_addr_slot_status(master->bus,
						   info->dyn_addr);
	if (slot_status == I3C_ADDR_SLOT_RSVD ||
	    slot_status == I3C_ADDR_SLOT_I2C_DEV)
		return -EINVAL;

	ret = i3c_master_getpid_locked(master, info);
	if (ret)
		return ret;

	ret = i3c_master_getbcr_locked(master, info);
	if (ret)
		return ret;

	ret = i3c_master_getdcr_locked(master, info);
	if (ret)
		return ret;

	if (info->bcr & I3C_BCR_MAX_DATA_SPEED_LIM) {
		ret = i3c_master_getmxds_locked(master, info);
		if (ret)
			return ret;
	}

	if (info->bcr & I3C_BCR_IBI_PAYLOAD)
		info->max_ibi_len = 1;

	i3c_master_getmrl_locked(master, info);
	i3c_master_getmwl_locked(master, info);

	if (info->bcr & I3C_BCR_HDR_CAP) {
		ret = i3c_master_gethdrcap_locked(master, info);
		if (ret)
			return ret;
	}

	return 0;
}

static int i3c_master_attach_i3c_dev(struct i3c_master_controller *master,
				     struct i3c_device *dev)
{
	int ret;

	/*
	 * We don't attach devices to the controller until they are
	 * addressable on the bus.
	 */
	if (!dev->info.static_addr && !dev->info.dyn_addr)
		return 0;

	dev->common.master = master;

	/* Do not attach the master device itself. */
	if (master->this == dev)
		return 0;

	if (!master->ops->attach_i3c_dev)
		return 0;

	ret = master->ops->attach_i3c_dev(dev);
	if (ret)
		dev->common.master = NULL;

	return ret;
}

static void i3c_master_reattach_i3c_dev(struct i3c_device *dev,
					u8 old_dyn_addr)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);

	if (master->ops->reattach_i3c_dev)
		master->ops->reattach_i3c_dev(dev, old_dyn_addr);

	if (old_dyn_addr)
		i3c_bus_set_addr_slot_status(master->bus, old_dyn_addr,
					     I3C_ADDR_SLOT_FREE);
}

static void i3c_master_detach_i3c_dev(struct i3c_device *dev)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);

	if (!master)
		return;

	/* Do not detach the master device itself. */
	if (master->this == dev)
		return;

	if (master->ops->detach_i3c_dev)
		master->ops->detach_i3c_dev(dev);

	dev->common.master = NULL;
}

static int i3c_master_attach_i2c_dev(struct i3c_master_controller *master,
				     struct i2c_device *dev)
{
	int ret;

	dev->common.master = master;

	if (!master->ops->attach_i2c_dev)
		return 0;

	ret = master->ops->attach_i2c_dev(dev);
	if (ret)
		dev->common.master = NULL;

	return ret;
}

static void i3c_master_detach_i2c_dev(struct i2c_device *dev)
{
	struct i3c_master_controller *master = i2c_device_get_master(dev);

	if (!master)
		return;

	if (master->ops->detach_i2c_dev)
		master->ops->detach_i2c_dev(dev);

	dev->common.master = NULL;
}

static void i3c_master_pre_assign_dyn_addr(struct i3c_device *dev)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);
	struct i3c_device_info info;
	int ret;

	if (!dev->init_dyn_addr || !dev->info.static_addr ||
	    dev->info.dyn_addr)
		return;

	ret = i3c_master_setdasa_locked(master, dev->info.static_addr,
					dev->init_dyn_addr);
	if (ret)
		return;

	ret = i3c_master_retrieve_dev_info(master, &info, dev->init_dyn_addr);
	if (ret)
		goto err_rstdaa;

	dev->info = info;

	i3c_master_reattach_i3c_dev(dev, 0);

	return;

err_rstdaa:
	i3c_master_rstdaa_locked(master, dev->init_dyn_addr);
}

static void
i3c_master_register_new_i3c_devs(struct i3c_master_controller *master)
{
	struct i3c_device *i3cdev;
	int ret;

	if (!master->init_done)
		return;

	i3c_bus_for_each_i3cdev(master->bus, i3cdev) {
		if (i3cdev->regfailed || device_is_registered(&i3cdev->dev) ||
		    !i3cdev->info.dyn_addr)
			continue;

		ret = device_add(&i3cdev->dev);
		if (ret) {
			dev_err(master->parent,
				"Failed to add I3C device (err = %d)\n", ret);
			i3cdev->regfailed = true;
		}
	}
}

/**
 * i3c_master_do_daa() - do a DAA (Dynamic Address Assignment)
 * @master: master doing the DAA
 *
 * This function is instantiating an I3C device object and adding it to the
 * I3C device list. All device information are automatically retrieved using
 * standard CCC commands.
 *
 * The I3C device object is returned in case the master wants to attach
 * private data to it using i3c_device_set_master_data().
 *
 * This function must be called with the bus lock held in write mode.
 *
 * Return: a 0 in case of success, an negative error code otherwise.
 */
int i3c_master_do_daa(struct i3c_master_controller *master)
{
	int ret;

	i3c_bus_maintenance_lock(master->bus);
	ret = master->ops->do_daa(master);
	i3c_bus_maintenance_unlock(master->bus);

	if (ret)
		return ret;

	i3c_bus_normaluse_lock(master->bus);
	i3c_master_register_new_i3c_devs(master);
	i3c_bus_normaluse_unlock(master->bus);

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_master_do_daa);

/**
 * i3c_master_bus_init() - initialize an I3C bus
 * @master: main master initializing the bus
 *
 * This function is following all initialisation steps described in the I3C
 * specification:
 *
 * 1/ Attach I2C and statically defined I3C devs to the master so that the
 *    master can fill its internal device table appropriately
 * 2/ Call master's ->bus_init() method to initialize the master controller.
 *    That's usually where the bus mode is selected (pure bus or mixed
 *    fast/slow bus)
 * 3/ Instruct all devices on the bus to drop their dynamic address. This is
 *    particularly important when the bus was previously configured by someone
 *    else (for example the bootloader)
 * 4/ Disable all slave events.
 * 5/ Pre-assign dynamic addresses requested by the FW with SETDASA for I3C
 *    devices that have a static address
 * 6/ Do a DAA (Dynamic Address Assignment) to assign dynamic addresses to all
 *    remaining I3C devices
 *
 * Once this is done, all I3C and I2C devices should be usable.
 *
 * Return: a 0 in case of success, an negative error code otherwise.
 */
static int i3c_master_bus_init(struct i3c_master_controller *master)
{
	struct i3c_device *i3cdev;
	struct i2c_device *i2cdev;
	int ret;

	/*
	 * First attach all devices with static definitions provided by the
	 * FW.
	 */
	i3c_bus_for_each_i2cdev(master->bus, i2cdev) {
		ret = i3c_master_attach_i2c_dev(master, i2cdev);
		if (ret)
			goto err_detach_devs;
	}

	i3c_bus_for_each_i3cdev(master->bus, i3cdev) {
		ret = i3c_master_attach_i3c_dev(master, i3cdev);
		if (ret)
			goto err_detach_devs;
	}

	/*
	 * Now execute the controller specific ->bus_init() routine, which
	 * might configure its internal logic to match the bus limitations.
	 */
	ret = master->ops->bus_init(master);
	if (ret)
		goto err_detach_devs;

	/*
	 * The master device should have been instantiated in ->bus_init(),
	 * complain if this was not the case.
	 */
	if (!master->this) {
		dev_err(master->parent,
			"master_set_info() was not called in ->bus_init()\n");
		ret = -EINVAL;
		goto err_bus_cleanup;
	}

	/*
	 * Reset all dynamic address that may have been assigned before
	 * (assigned by the bootloader for example).
	 */
	ret = i3c_master_rstdaa_locked(master, I3C_BROADCAST_ADDR);
	if (ret)
		goto err_bus_cleanup;

	/* Disable all slave events before starting DAA. */
	ret = i3c_master_disec_locked(master, I3C_BROADCAST_ADDR,
				      I3C_CCC_EVENT_SIR | I3C_CCC_EVENT_MR |
				      I3C_CCC_EVENT_HJ);
	if (ret)
		goto err_bus_cleanup;

	/*
	 * Pre-assign dynamic address and retrieve device information if
	 * needed.
	 */
	i3c_bus_for_each_i3cdev(master->bus, i3cdev)
		i3c_master_pre_assign_dyn_addr(i3cdev);

	ret = i3c_master_do_daa(master);
	if (ret)
		goto err_rstdaa;

	return 0;

err_rstdaa:
	i3c_master_rstdaa_locked(master, I3C_BROADCAST_ADDR);

err_bus_cleanup:
	if (master->ops->bus_cleanup)
		master->ops->bus_cleanup(master);

err_detach_devs:
	i3c_bus_for_each_i3cdev(master->bus, i3cdev)
		i3c_master_detach_i3c_dev(i3cdev);

	i3c_bus_for_each_i2cdev(master->bus, i2cdev)
		i3c_master_detach_i2c_dev(i2cdev);

	return ret;
}

static void i3c_master_bus_cleanup(struct i3c_master_controller *master)
{
	struct i3c_device *i3cdev;
	struct i2c_device *i2cdev;

	if (master->ops->bus_cleanup)
		master->ops->bus_cleanup(master);

	i3c_bus_for_each_i3cdev(master->bus, i3cdev)
		i3c_master_detach_i3c_dev(i3cdev);

	i3c_bus_for_each_i2cdev(master->bus, i2cdev)
		i3c_master_detach_i2c_dev(i2cdev);
}

static struct i3c_device *
i3c_master_search_i3c_dev_by_pid(struct i3c_master_controller *master, u64 pid)
{
	struct i3c_device *i3cdev;

	i3c_bus_for_each_i3cdev(master->bus, i3cdev) {
		if (i3cdev->info.pid == pid)
			return i3cdev;
	}

	return NULL;
}

/**
 * i3c_master_add_i3c_dev_locked() - add an I3C slave to the bus
 * @master: master used to send frames on the bus
 * @addr: I3C slave dynamic address assigned to the device
 *
 * This function is instantiating an I3C device object and adding it to the
 * I3C device list. All device information are automatically retrieved using
 * standard CCC commands.
 *
 * The I3C device object is returned in case the master wants to attach
 * private data to it using i3c_device_set_master_data().
 *
 * This function must be called with the bus lock held in write mode.
 *
 * Return: a 0 in case of success, an negative error code otherwise.
 */
int i3c_master_add_i3c_dev_locked(struct i3c_master_controller *master,
				  u8 addr)
{
	u8 old_dyn_addr, expected_dyn_addr;
	enum i3c_addr_slot_status status;
	struct i3c_device *i3cdev;
	struct i3c_device_info info;
	int ret;

	if (!master)
		return -EINVAL;

	status = i3c_bus_get_addr_slot_status(master->bus, addr);
	if (status != I3C_ADDR_SLOT_FREE)
		return -EINVAL;

	ret = i3c_master_retrieve_dev_info(master, &info, addr);
	if (ret)
		return ret;

	i3cdev = i3c_master_search_i3c_dev_by_pid(master, info.pid);
	if (!i3cdev) {
		i3cdev = i3c_master_alloc_i3c_dev(master, &info,
						  &i3c_device_type);
		if (IS_ERR(i3cdev))
			return PTR_ERR(i3cdev);

		list_add_tail(&i3cdev->common.node, &master->bus->devs.i3c);
	}

	old_dyn_addr = i3cdev->info.dyn_addr;
	i3cdev->info.dyn_addr = addr;

	if (!i3cdev->common.master) {
		ret = i3c_master_attach_i3c_dev(master, i3cdev);
		if (ret)
			goto err_put_dev;
	} else {
		i3c_master_reattach_i3c_dev(i3cdev, old_dyn_addr);
	}

	/*
	 * Depending on our previous state, the expected dynamic address might
	 * differ:
	 * - if the device already had a dynamic address assigned, let's try to
	 *   re-apply this one
	 * - if the device did not have a dynamic address and the firmware
	 *   requested a specific address, pick this one
	 * - in any other case, keep the address automatically assigned by the
	 *   master
	 */
	if (old_dyn_addr)
		expected_dyn_addr = old_dyn_addr;
	else if (i3cdev->init_dyn_addr)
		expected_dyn_addr = i3cdev->init_dyn_addr;
	else
		expected_dyn_addr = i3cdev->info.dyn_addr;

	if (i3cdev->info.dyn_addr != expected_dyn_addr) {
		/*
		 * Try to apply the expected dynamic address. If it fails, keep
		 * the address assigned by the master.
		 */
		ret = i3c_master_setnewda_locked(master,
						 i3cdev->info.dyn_addr,
						 expected_dyn_addr);
		if (!ret) {
			old_dyn_addr = i3cdev->info.dyn_addr;
			i3cdev->info.dyn_addr = expected_dyn_addr;
			i3c_master_reattach_i3c_dev(i3cdev, old_dyn_addr);
		}
	}

	return 0;

err_put_dev:
	list_del(&i3cdev->common.node);
	put_device(&i3cdev->dev);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_master_add_i3c_dev_locked);

#define OF_I3C_REG1_IS_I2C_DEV			BIT(31)

static int of_i3c_master_add_i2c_dev(struct i3c_master_controller *master,
				     struct device_node *node, u32 *reg)
{
	struct device *dev = master->parent;
	struct i2c_board_info info = { };
	struct i2c_device *i2cdev;
	/* LVR is encoded in the lowest byte of reg[1]. */
	u8 lvr = reg[1];
	int ret;

	ret = of_i2c_get_board_info(master->parent, node, &info);
	if (ret)
		return ret;

	/*
	 * We do not register the I2C device here, because the bus is not
	 * necessarily ready to transmit I2C frames, and the I2C adapter has
	 * not been registered yet.
	 * This is done in i3c_master_i2c_adapter_init() once everything is
	 * ready.
	 */
	i2cdev = i3c_master_alloc_i2c_dev(master, &info, lvr);
	if (IS_ERR(i2cdev)) {
		dev_err(dev, "Failed to allocate device %04x\n", info.addr);
		return ret;
	}

	if (lvr & I3C_LVR_I2C_FM_MODE)
		master->bus->scl_rate.i2c = I3C_BUS_I2C_FM_SCL_RATE;

	list_add_tail(&i2cdev->common.node, &master->bus->devs.i2c);

	return 0;
}

static int of_i3c_master_add_i3c_dev(struct i3c_master_controller *master,
				     struct device_node *node, u32 *reg)
{
	struct i3c_device_info info = { };
	enum i3c_addr_slot_status addrstatus;
	struct i3c_device *i3cdev;
	u32 init_dyn_addr = 0;

	if (reg[0]) {
		if (reg[0] > I3C_MAX_ADDR)
			return -EINVAL;

		addrstatus = i3c_bus_get_addr_slot_status(master->bus, reg[0]);
		if (addrstatus != I3C_ADDR_SLOT_FREE)
			return -EINVAL;
	}

	info.static_addr = reg[0];

	if (!of_property_read_u32(node, "assigned-address", &init_dyn_addr)) {
		if (init_dyn_addr > I3C_MAX_ADDR)
			return -EINVAL;

		addrstatus = i3c_bus_get_addr_slot_status(master->bus,
							  init_dyn_addr);
		if (addrstatus != I3C_ADDR_SLOT_FREE)
			return -EINVAL;
	}

	info.pid = ((u64)reg[1] << 32) | reg[2];

	if ((info.pid & GENMASK_ULL(63, 48)) ||
	    I3C_PID_RND_LOWER_32BITS(info.pid))
		return -EINVAL;

	i3cdev = i3c_master_alloc_i3c_dev(master, &info, &i3c_device_type);
	if (IS_ERR(i3cdev))
		return PTR_ERR(i3cdev);

	i3cdev->init_dyn_addr = init_dyn_addr;
	i3cdev->dev.of_node = node;
	list_add_tail(&i3cdev->common.node, &master->bus->devs.i3c);

	return 0;
}

static int of_i3c_master_add_dev(struct i3c_master_controller *master,
				 struct device_node *node)
{
	u32 reg[3];
	int ret;

	if (!master || !node)
		return -EINVAL;

	ret = of_property_read_u32_array(node, "reg", reg, ARRAY_SIZE(reg));
	if (ret)
		return ret;

	if (reg[1] & OF_I3C_REG1_IS_I2C_DEV)
		ret = of_i3c_master_add_i2c_dev(master, node, reg);
	else
		ret = of_i3c_master_add_i3c_dev(master, node, reg);

	return ret;
}

static int of_populate_i3c_bus(struct i3c_master_controller *master)
{
	struct device *dev = &master->bus->dev;
	struct device_node *i3cbus_np = dev->of_node;
	struct device_node *node;
	int ret;
	u32 val;

	if (!i3cbus_np)
		return 0;

	for_each_available_child_of_node(i3cbus_np, node) {
		ret = of_i3c_master_add_dev(master, node);
		if (ret)
			return ret;
	}

	/*
	 * The user might want to limit I2C and I3C speed in case some devices
	 * on the bus are not supporting typical rates, or if the bus topology
	 * prevents it from using max possible rate.
	 */
	if (!of_property_read_u32(i3cbus_np, "i2c-scl-hz", &val))
		master->bus->scl_rate.i2c = val;

	if (!of_property_read_u32(i3cbus_np, "i3c-scl-hz", &val))
		master->bus->scl_rate.i3c = val;

	return 0;
}

static int i3c_master_i2c_adapter_xfer(struct i2c_adapter *adap,
				       struct i2c_msg *xfers, int nxfers)
{
	struct i3c_master_controller *master = i2c_adapter_to_i3c_master(adap);
	struct i2c_device *dev;
	int i, ret;
	u16 addr;

	if (!xfers || !master || nxfers <= 0)
		return -EINVAL;

	if (!master->ops->i2c_xfers)
		return -ENOTSUPP;

	/* Doing transfers to different devices is not supported. */
	addr = xfers[0].addr;
	for (i = 1; i < nxfers; i++) {
		if (addr != xfers[i].addr)
			return -ENOTSUPP;
	}

	i3c_bus_normaluse_lock(master->bus);
	dev = i3c_master_find_i2c_dev_by_addr(master, addr);
	if (!dev)
		ret = -ENOENT;
	else
		ret = master->ops->i2c_xfers(dev, xfers, nxfers);
	i3c_bus_normaluse_unlock(master->bus);

	return ret ? ret : nxfers;
}

static u32 i3c_master_i2c_functionalities(struct i2c_adapter *adap)
{
	struct i3c_master_controller *master = i2c_adapter_to_i3c_master(adap);

	return master->ops->i2c_funcs(master);
}

static const struct i2c_algorithm i3c_master_i2c_algo = {
	.master_xfer = i3c_master_i2c_adapter_xfer,
	.functionality = i3c_master_i2c_functionalities,
};

static int i3c_master_i2c_adapter_init(struct i3c_master_controller *master)
{
	struct i2c_adapter *adap = i3c_master_to_i2c_adapter(master);
	struct i2c_device *i2cdev;
	int ret;

	adap->dev.parent = master->parent;
	adap->owner = master->parent->driver->owner;
	adap->algo = &i3c_master_i2c_algo;
	strncpy(adap->name, dev_name(master->parent), sizeof(adap->name));

	/* FIXME: Should we allow i3c masters to override these values? */
	adap->timeout = 1000;
	adap->retries = 3;

	ret = i2c_add_adapter(adap);
	if (ret)
		return ret;

	/*
	 * We silently ignore failures here. The bus should keep working
	 * correctly even if one or more i2c devices are not registered.
	 */
	i3c_bus_for_each_i2cdev(master->bus, i2cdev)
		i2cdev->client = i2c_new_device(adap, &i2cdev->info);

	return 0;
}

static void i3c_master_i2c_adapter_cleanup(struct i3c_master_controller *master)
{
	i2c_del_adapter(&master->i2c);
}

static void i3c_master_unregister_i3c_devs(struct i3c_master_controller *master)
{
	struct i3c_device *i3cdev;

	i3c_bus_for_each_i3cdev(master->bus, i3cdev) {
		if (device_is_registered(&i3cdev->dev))
			device_del(&i3cdev->dev);
	}
}

/**
 * i3c_master_queue_ibi() - Queue an IBI
 * @dev: the device this IBI is coming from
 * @slot: the IBI slot used to store the payload
 *
 * Queue an IBI to the controller workqueue. The IBI handler attached to
 * the dev will be called from a workqueue context.
 */
void i3c_master_queue_ibi(struct i3c_device *dev, struct i3c_ibi_slot *slot)
{
	atomic_inc(&dev->ibi->pending_ibis);
	queue_work(dev->common.master->wq, &slot->work);
}
EXPORT_SYMBOL_GPL(i3c_master_queue_ibi);

static void i3c_master_handle_ibi(struct work_struct *work)
{
	struct i3c_ibi_slot *slot = container_of(work, struct i3c_ibi_slot,
						 work);
	struct i3c_device *dev = slot->dev;
	struct i3c_master_controller *master = i3c_device_get_master(dev);
	struct i3c_ibi_payload payload;

	payload.data = slot->data;
	payload.len = slot->len;

	dev->ibi->handler(dev, &payload);
	master->ops->recycle_ibi_slot(dev, slot);
	if (atomic_dec_and_test(&dev->ibi->pending_ibis))
		complete(&dev->ibi->all_ibis_handled);
}

static void i3c_master_init_ibi_slot(struct i3c_device *dev,
				     struct i3c_ibi_slot *slot)
{
	slot->dev = dev;
	INIT_WORK(&slot->work, i3c_master_handle_ibi);
}

struct i3c_generic_ibi_slot {
	struct list_head node;
	struct i3c_ibi_slot base;
};

struct i3c_generic_ibi_pool {
	spinlock_t lock;
	unsigned int num_slots;
	struct list_head free_slots;
	struct list_head pending;
};

/**
 * i3c_generic_ibi_free_pool() - Free a generic IBI pool
 * @pool: the IBI pool to free
 *
 * Free all IBI slots allated by a generic IBI pool.
 */
void i3c_generic_ibi_free_pool(struct i3c_generic_ibi_pool *pool)
{
	struct i3c_generic_ibi_slot *slot;
	unsigned int nslots = 0;

	while (!list_empty(&pool->free_slots)) {
		slot = list_first_entry(&pool->free_slots,
					struct i3c_generic_ibi_slot, node);
		list_del(&slot->node);
		kfree(slot->base.data);
		kfree(slot);
		nslots++;
	}

	/*
	 * If the number of freed slots is not equal to the number of allocated
	 * slots we have a leak somewhere.
	 */
	WARN_ON(nslots != pool->num_slots);
}
EXPORT_SYMBOL_GPL(i3c_generic_ibi_free_pool);

/**
 * i3c_generic_ibi_alloc_pool() - Create a generic IBI pool
 * @dev: the device this pool will be used for
 * @req: IBI setup request describing what the device driver expects
 *
 * Create a generic IBI pool based on the information provided in @req.
 *
 * Return: a valid IBI pool in case of success, an ERR_PTR() otherwise.
 */
struct i3c_generic_ibi_pool *
i3c_generic_ibi_alloc_pool(struct i3c_device *dev,
			   const struct i3c_ibi_setup *req)
{
	struct i3c_generic_ibi_pool *pool;
	struct i3c_generic_ibi_slot *slot;
	unsigned int i;
	int ret;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&pool->lock);
	INIT_LIST_HEAD(&pool->free_slots);
	INIT_LIST_HEAD(&pool->pending);

	for (i = 0; i < req->num_slots; i++) {
		slot = kzalloc(sizeof(*slot), GFP_KERNEL);
		if (!slot)
			return ERR_PTR(-ENOMEM);

		i3c_master_init_ibi_slot(dev, &slot->base);

		if (req->max_payload_len) {
			slot->base.data = kzalloc(req->max_payload_len,
						  GFP_KERNEL);
			if (!slot->base.data) {
				kfree(slot);
				ret = -ENOMEM;
				goto err_free_pool;
			}
		}

		list_add_tail(&slot->node, &pool->free_slots);
		pool->num_slots++;
	}

	return pool;

err_free_pool:
	i3c_generic_ibi_free_pool(pool);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(i3c_generic_ibi_alloc_pool);

/**
 * i3c_generic_ibi_get_free_slot() - Get a free slot from a generic IBI pool
 * @pool: the pool to query an IBI slot on
 *
 * Search for a free slot in a generic IBI pool.
 * The slot should be returned to the pool using i3c_generic_ibi_recycle_slot()
 * when it's no longer needed.
 *
 * Return: a pointer to a free slot, or NULL if there's no free slot available.
 */
struct i3c_ibi_slot *
i3c_generic_ibi_get_free_slot(struct i3c_generic_ibi_pool *pool)
{
	struct i3c_generic_ibi_slot *slot;
	unsigned long flags;

	spin_lock_irqsave(&pool->lock, flags);
	slot = list_first_entry_or_null(&pool->free_slots,
					struct i3c_generic_ibi_slot, node);
	if (slot)
		list_del(&slot->node);
	spin_unlock_irqrestore(&pool->lock, flags);

	return slot ? &slot->base : NULL;
}
EXPORT_SYMBOL_GPL(i3c_generic_ibi_get_free_slot);

/**
 * i3c_generic_ibi_recycle_slot() - Return a slot to a generic IBI pool
 * @pool: the pool to return the IBI slot to
 * @s: IBI slot to recycle
 *
 * Add an IBI slot back to its generic IBI pool. Should be called from the
 * master driver struct_master_controller_ops->recycle_ibi() method.
 */
void i3c_generic_ibi_recycle_slot(struct i3c_generic_ibi_pool *pool,
				  struct i3c_ibi_slot *s)
{
	struct i3c_generic_ibi_slot *slot;
	unsigned long flags;

	if (!s)
		return;

	slot = container_of(s, struct i3c_generic_ibi_slot, base);
	spin_lock_irqsave(&pool->lock, flags);
	list_add_tail(&slot->node, &pool->free_slots);
	spin_unlock_irqrestore(&pool->lock, flags);
}
EXPORT_SYMBOL_GPL(i3c_generic_ibi_recycle_slot);

static void i3c_master_destroy_bus(struct i3c_master_controller *master)
{
	i3c_bus_unregister(master->bus);
}

static int i3c_master_create_bus(struct i3c_master_controller *master)
{
	struct i3c_bus *i3cbus;
	int ret;

	i3cbus = i3c_bus_create(master->parent);
	if (IS_ERR(i3cbus))
		return PTR_ERR(i3cbus);

	master->bus = i3cbus;

	if (i3cbus->dev.of_node) {
		ret = of_populate_i3c_bus(master);
		if (ret)
			goto err_destroy_bus;
	}

	ret = i3c_bus_register(i3cbus);
	if (ret)
		goto err_destroy_bus;

	return 0;

err_destroy_bus:
	i3c_bus_unref(i3cbus);

	return ret;
}

static int i3c_master_check_ops(const struct i3c_master_controller_ops *ops)
{
	if (!ops || !ops->bus_init || !ops->priv_xfers ||
	    !ops->send_ccc_cmd || !ops->do_daa || !ops->i2c_xfers ||
	    !ops->i2c_funcs)
		return -EINVAL;

	if (ops->request_ibi &&
	    (!ops->enable_ibi || !ops->disable_ibi || !ops->free_ibi ||
	     !ops->recycle_ibi_slot))
		return -EINVAL;

	return 0;
}

/**
 * i3c_master_register() - register an I3C master
 * @master: master used to send frames on the bus
 * @parent: the parent device (the one that provides this I3C master
 *	    controller)
 * @ops: the master controller operations
 * @secondary: true if you are registering a secondary master. Will return
 *	       -ENOTSUPP if set to true since secondary masters are not yet
 *	       supported
 *
 * This function takes care of everything for you:
 *
 * - creates and initializes the I3C bus
 * - populates the bus with static I2C devs if @parent->of_node is not
 *   NULL
 * - registers all I3C devices added by the controller during bus
 *   initialization
 * - registers the I2C adapter and all I2C devices
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_register(struct i3c_master_controller *master,
			struct device *parent,
			const struct i3c_master_controller_ops *ops,
			bool secondary)
{
	int ret;

	/* We do not support secondary masters yet. */
	if (secondary)
		return -ENOTSUPP;

	ret = i3c_master_check_ops(ops);
	if (ret)
		return ret;

	master->parent = parent;
	master->ops = ops;
	master->secondary = secondary;

	ret = i3c_master_create_bus(master);
	if (ret)
		return ret;

	master->wq = alloc_workqueue("%s", 0, 0, dev_name(parent));
	if (!master->wq) {
		ret = -ENOMEM;
		goto err_destroy_bus;
	}

	ret = i3c_master_bus_init(master);
	if (ret)
		goto err_destroy_wq;

	/*
	 * Expose our I3C bus as an I2C adapter so that I2C devices are exposed
	 * through the I2C subsystem.
	 */
	ret = i3c_master_i2c_adapter_init(master);
	if (ret)
		goto err_cleanup_bus;

	/*
	 * We're done initializing the bus and the controller, we can now
	 * register I3C devices dicovered during the initial DAA.
	 */
	master->init_done = true;
	i3c_bus_normaluse_lock(master->bus);
	i3c_master_register_new_i3c_devs(master);
	i3c_bus_normaluse_unlock(master->bus);

	return 0;

err_cleanup_bus:
	i3c_master_bus_cleanup(master);

err_destroy_wq:
	destroy_workqueue(master->wq);

err_destroy_bus:
	i3c_master_destroy_bus(master);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_master_register);

/**
 * i3c_master_unregister() - unregister an I3C master
 * @master: master used to send frames on the bus
 *
 * Basically undo everything done in i3c_master_register().
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_unregister(struct i3c_master_controller *master)
{
	i3c_master_i2c_adapter_cleanup(master);
	i3c_master_unregister_i3c_devs(master);
	i3c_master_bus_cleanup(master);
	destroy_workqueue(master->wq);
	i3c_master_destroy_bus(master);

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_master_unregister);
