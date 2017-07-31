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

#include <linux/slab.h>

#include "internals.h"

static inline struct i3c_master_controller *
i2c_adapter_to_i3c_master(struct i2c_adapter *adap)
{
	return container_of(adap, struct i3c_master_controller, i2c);
}

static inline struct i2c_adapter *
i3c_master_to_i2c_adapter(struct i3c_master_controller *master)
{
	return &master->i2c;
}

static void i3c_i2c_dev_init(struct i3c_master_controller *master,
			     struct i3c_i2c_dev *dev, bool i2cdev)
{
	dev->bus = master->bus;
	dev->master = master;
}

static struct i2c_device *
i3c_master_alloc_i2c_dev(struct i3c_master_controller *master,
			 const struct i2c_board_info *info, u8 lvr)
{
	struct i2c_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	i3c_i2c_dev_init(master, &dev->common, true);
	dev->info = *info;
	dev->lvr = lvr;
	dev->info.of_node = of_node_get(info->of_node);
	i3c_bus_set_addr_slot_status(master->bus, info->addr,
				     I3C_ADDR_SLOT_I2C_DEV);

	return dev;
}

static void i3c_master_init_i3c_dev(struct i3c_master_controller *master,
				    struct i3c_device *dev,
				    const struct i3c_device_info *info,
				    const struct device_type *type)
{
	i3c_i2c_dev_init(master, &dev->common, false);
	dev->dev.parent = &master->bus->dev;
	dev->dev.type = type;
	dev->dev.bus = &i3c_bus_type;
	dev->info = *info;
	dev_set_name(&dev->dev, "%d-%llx", master->bus->id, info->pid);
}

static int i3c_master_send_ccc_cmd_locked(struct i3c_master_controller *master,
					  struct i3c_ccc_cmd *cmd)
{
	if (WARN_ON(!rwsem_is_locked(&master->bus->lock)))
		return -EINVAL;

	if (!cmd || !master)
		return -EINVAL;

	if (!master->ops->send_ccc_cmd)
		return -ENOTSUPP;

	if ((cmd->id & I3C_CCC_DIRECT)) {
		enum i3c_addr_slot_status status;
		int i;

		if (!cmd->dests || !cmd->ndests)
			return -EINVAL;

		for (i = 0; i < cmd->ndests; i++) {
			status = i3c_bus_get_addr_slot_status(master->bus,
							cmd->dests[i].addr);
			if (status != I3C_ADDR_SLOT_I3C_DEV)
				return -EINVAL;
		}
	}

	if (master->ops->supports_ccc_cmd &&
	    !master->ops->supports_ccc_cmd(master, cmd))
		return -ENOTSUPP;

	return master->ops->send_ccc_cmd(master, cmd);
}

int i3c_master_send_hdr_cmds_locked(struct i3c_master_controller *master,
				    const struct i3c_hdr_cmd *cmds, int ncmds)
{
	int i;

	if (!cmds || !master || ncmds <= 0)
		return -EINVAL;

	if (!master->ops->send_hdr_cmds)
		return -ENOTSUPP;

	for (i = 0; i < ncmds; i++) {
		if (!(master->base.info.hdr_cap & BIT(cmds->mode)))
			return -ENOTSUPP;
	}

	return master->ops->send_hdr_cmds(master, cmds, ncmds);
}

/**
 * i3c_master_send_hdr_cmds() - send HDR commands on the I3C bus
 *
 * @master: master used to send frames on the bus.
 * @cmds: array of HDR commands.
 * @ncmds: number of commands to send.
 *
 * Send one or several HDR commands.
 *
 * This function can sleep and thus cannot be called in atomic context.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_send_hdr_cmds(struct i3c_master_controller *master,
			     const struct i3c_hdr_cmd *cmds, int ncmds)
{
	int ret;

	i3c_bus_lock(master->bus, false);
	ret = i3c_master_send_hdr_cmds_locked(master, cmds, ncmds);
	i3c_bus_unlock(master->bus, false);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_master_send_hdr_cmds);

int i3c_master_do_priv_xfers_locked(struct i3c_master_controller *master,
				    const struct i3c_priv_xfer *xfers,
				    int nxfers)
{
	int i;

	if (!xfers || !master || nxfers <= 0)
		return -EINVAL;

	if (!master->ops->priv_xfers)
		return -ENOTSUPP;

	for (i = 0; i < nxfers; i++) {
		enum i3c_addr_slot_status status;

		status = i3c_bus_get_addr_slot_status(master->bus,
						      xfers[i].addr);
		if (status != I3C_ADDR_SLOT_I3C_DEV)
			return -EINVAL;
	}

	return master->ops->priv_xfers(master, xfers, nxfers);
}

/**
 * i3c_master_do_priv_xfers() - do SDR private transfers on the I3C bus
 *
 * @master: master used to send frames on the bus
 * @xfers: array of SDR private transfers
 * @nxfers: number of transfers
 *
 * Do one or several private SDR I3C transfers.
 *
 * This function can sleep and thus cannot be called in atomic context.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_do_priv_xfers(struct i3c_master_controller *master,
			     const struct i3c_priv_xfer *xfers,
			     int nxfers)
{
	int ret;

	i3c_bus_lock(master->bus, false);
	ret = i3c_master_do_priv_xfers_locked(master, xfers, nxfers);
	i3c_bus_unlock(master->bus, false);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_master_do_priv_xfers);

/**
 * i3c_master_do_i2c_xfers() - do I2C transfers on the I3C bus
 *
 * @master: master used to send frames on the bus
 * @xfers: array of I2C transfers
 * @nxfers: number of transfers
 *
 * Does one or several I2C transfers.
 *
 * This function can sleep and thus cannot be called in atomic context.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_do_i2c_xfers(struct i3c_master_controller *master,
			    const struct i2c_msg *xfers,
			    int nxfers)
{
	int ret, i;

	if (!xfers || !master || nxfers <= 0)
		return -EINVAL;

	if (!master->ops->i2c_xfers)
		return -ENOTSUPP;

	i3c_bus_lock(master->bus, false);

	for (i = 0; i < nxfers; i++) {
		enum i3c_addr_slot_status status;

		status = i3c_bus_get_addr_slot_status(master->bus,
						      xfers[i].addr);
		if (status != I3C_ADDR_SLOT_I2C_DEV) {
			ret = -EINVAL;
			goto out;
		}
	}

	ret = master->ops->i2c_xfers(master, xfers, nxfers);

out:
	i3c_bus_unlock(master->bus, false);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_master_do_i2c_xfers);

/**
 * i3c_master_get_free_addr() - get a free address on the bus
 *
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

/**
 * i3c_master_set_info() - set master device information
 *
 * @master: master used to send frames on the bus
 * @info: I3C device information
 *
 * Set master device info. This should be done in
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
 * This function must be called with the bus lock held in write mode.
 *
 * Return: 0 if @info contains valid information (not every piece of
 * information can be checked, but we can at least make sure @info->dyn_addr
 * and @info->bcr are correct), -EINVAL otherwise.
 */
int i3c_master_set_info(struct i3c_master_controller *master,
			const struct i3c_device_info *info)
{
	if (!i3c_bus_dev_addr_is_avail(master->bus, info->dyn_addr))
		return -EINVAL;

	if (I3C_BCR_DEVICE_ROLE(info->bcr) == I3C_BCR_I3C_MASTER &&
	    master->secondary)
		return -EINVAL;

	i3c_master_init_i3c_dev(master, &master->base, info, &i3c_master_type);
	i3c_bus_set_addr_slot_status(master->bus, info->dyn_addr,
				     I3C_ADDR_SLOT_I3C_DEV);

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_master_set_info);

static struct i3c_device *
i3c_master_alloc_i3c_dev(struct i3c_master_controller *master,
			 const struct i3c_device_info *info)
{
	struct i3c_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return ERR_PTR(-ENOMEM);

	i3c_master_init_i3c_dev(master, dev, info, &i3c_device_type);

	return dev;
}

/**
 * i3c_master_rstdaa_locked() - reset dev(s) dynamic address
 *
 * @master: master used to send frames on the bus
 * @addr: a valid I3C device address or %I3C_BROADCAST_ADDR
 *
 * Send a RSTDAA CCC command to ask a specific slave (or all slave if @addr is
 * %I3C_BROADCAST_ADDR) to drop their dynamic address.
 *
 * This function must be called with the bus lock held in write mode.
 *
 * Return: 0 in case of success, a negative error code otherwise.
 */
int i3c_master_rstdaa_locked(struct i3c_master_controller *master, u8 addr)
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
EXPORT_SYMBOL_GPL(i3c_master_rstdaa_locked);

/**
 * i3c_master_entdaa_locked() - start a DAA (Dynamic Address Assignment)
 *				procedure
 *
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
 *
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
			    const struct i3c_ccc_events *evts)
{
	struct i3c_ccc_events events = *evts;
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
 * i3c_master_defslvs_locked() - send a DEFSLVS CCC command
 *
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
	int ndevs, ret;

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
			   (ndevs * sizeof(struct i3c_ccc_dev_desc));
	defslvs = kzalloc(dest.payload.len, GFP_KERNEL);
	if (!defslvs)
		return -ENOMEM;

	dest.payload.data = defslvs;

	defslvs->count = ndevs + 1;
	defslvs->master.bcr = master->base.info.bcr;
	defslvs->master.dcr = master->base.info.dcr;
	defslvs->master.dyn_addr = master->base.info.dyn_addr;
	defslvs->master.static_addr = master->base.info.static_addr;

	desc = defslvs->slaves;
	i3c_bus_for_each_i2cdev(bus, i2cdev) {
		desc->lvr = i2cdev->lvr;
		desc->static_addr = i2cdev->info.addr;
		desc++;
	}

	i3c_bus_for_each_i3cdev(bus, i3cdev) {
		desc->bcr = i3cdev->info.bcr;
		desc->dcr = i3cdev->info.dcr;
		desc->dyn_addr = i3cdev->info.dyn_addr;
		desc->static_addr = i3cdev->info.static_addr;
		desc++;
	}

	ret = i3c_master_send_ccc_cmd_locked(master, &cmd);
	kfree(defslvs);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_master_defslvs_locked);

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

	ret = i3c_master_getmxds_locked(master, info);
	if (ret && (info->bcr & I3C_BCR_MAX_DATA_SPEED_LIM))
		return -EINVAL;

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

/**
 * i3c_master_add_i3c_dev_locked() - add an I3C slave to the bus
 *
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
 * Return: a pointer to a &struct i3c_device object in case of success,
 * an ERR_PTR() otherwise.
 */
struct i3c_device *
i3c_master_add_i3c_dev_locked(struct i3c_master_controller *master, u8 addr)
{
	enum i3c_addr_slot_status status;
	struct i3c_device *i3cdev;
	struct i3c_device_info info;
	int ret;

	if (!master)
		return ERR_PTR(-EINVAL);

	status = i3c_bus_get_addr_slot_status(master->bus, addr);
	if (status != I3C_ADDR_SLOT_FREE)
		return ERR_PTR(-EINVAL);

	i3c_bus_set_addr_slot_status(master->bus, addr, I3C_ADDR_SLOT_I3C_DEV);

	ret = i3c_master_retrieve_dev_info(master, &info, addr);
	if (ret)
		goto err_release_addr;

	i3cdev = i3c_master_alloc_i3c_dev(master, &info);
	if (IS_ERR(i3cdev)) {
		ret = PTR_ERR(i3cdev);
		goto err_release_addr;
	}

	list_add_tail(&i3cdev->common.node, &master->bus->devs.i3c);

	return i3cdev;

err_release_addr:
	i3c_bus_set_addr_slot_status(master->bus, addr, I3C_ADDR_SLOT_FREE);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(i3c_master_add_i3c_dev_locked);

static int of_i3c_master_add_dev(struct i3c_master_controller *master,
				 struct device_node *node)
{
	struct device *dev = master->base.dev.parent;
	struct i2c_board_info info = { };
	struct i2c_device *i2cdev;
	u32 lvr, addr;
	int ret;

	if (!master || !node)
		return -EINVAL;

	/*
	 * This node is not describing an I2C device, skip it.
	 * We only add I2C devices here (i.e. nodes with an i3c-lvr property).
	 * I3C devices will be discovered during DAA, even if they have a
	 * static address.
	 */
	if (of_property_read_u32(node, "reg", &addr) ||
	    of_property_read_u32(node, "i3c-lvr", &lvr))
		return 0;

	ret = of_i2c_get_board_info(master->base.dev.parent, node, &info);
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
		dev_err(dev, "Failed to allocate device %02x\n", addr);
		return ret;
	}

	if (lvr & I3C_LVR_I2C_FM_MODE)
		master->bus->scl_rate.i2c = I3C_BUS_I2C_FM_SCL_RATE;

	list_add_tail(&i2cdev->common.node, &master->bus->devs.i2c);

	return 0;
}

static void i3c_master_remove_devs(struct i3c_master_controller *master)
{
	while (!list_empty(&master->bus->devs.i2c)) {
		struct i2c_device *i2cdev;

		i2cdev = list_first_entry(&master->bus->devs.i2c,
					  struct i2c_device, common.node);
		list_del(&i2cdev->common.node);
		of_node_put(i2cdev->info.of_node);
		kfree(i2cdev);
	}

	while (!list_empty(&master->bus->devs.i3c)) {
		struct i3c_device *i3cdev;

		i3cdev = list_first_entry(&master->bus->devs.i3c,
					  struct i3c_device, common.node);
		list_del(&i3cdev->common.node);
		of_node_put(i3cdev->dev.of_node);
		kfree(i3cdev);
	}
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
			goto err_remove_devs;
	}

	/*
	 * The user might want to limit I2C and I3C speed in case some devices
	 * on the bus are not supporting typical rates, or if the bus topology
	 * prevents it from using max possible rate.
	 */
	if (!of_property_read_u32(i3cbus_np, "i2c-scl-frequency", &val))
		master->bus->scl_rate.i2c = val;

	if (!of_property_read_u32(i3cbus_np, "i3c-scl-frequency", &val))
		master->bus->scl_rate.i3c = val;

	return 0;

err_remove_devs:
	i3c_master_remove_devs(master);

	return ret;
}

static int i3c_master_i2c_adapter_xfer(struct i2c_adapter *adap,
				       struct i2c_msg *xfers, int nxfers)
{
	struct i3c_master_controller *master = i2c_adapter_to_i3c_master(adap);
	int i, ret;

	for (i = 0; i < nxfers; i++) {
		enum i3c_addr_slot_status status;

		status = i3c_bus_get_addr_slot_status(master->bus,
						      xfers[i].addr);
		if (status != I3C_ADDR_SLOT_I2C_DEV)
			return -EINVAL;
	}

	ret = i3c_master_do_i2c_xfers(master, xfers, nxfers);
	if (ret)
		return ret;

	return nxfers;
}

static u32 i3c_master_i2c_functionalities(struct i2c_adapter *adap)
{
	return I2C_FUNC_SMBUS_EMUL | I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR;
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
	strncpy(adap->name, dev_name(master->base.dev.parent),
		sizeof(adap->name));

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
			device_unregister(&i3cdev->dev);
	}
}

static int i3c_master_register_i3c_devs(struct i3c_master_controller *master)
{
	struct i3c_device *i3cdev;
	int ret;

	i3c_bus_for_each_i3cdev(master->bus, i3cdev) {
		ret = device_register(&i3cdev->dev);
		if (ret)
			goto err_unregister_devs;
	}

	return 0;

err_unregister_devs:
	i3c_master_unregister_i3c_devs(master);

	return ret;
}

static int i3c_master_init_bus(struct i3c_master_controller *master)
{
	int ret;

	if (!master->ops->bus_init)
		return 0;

	/*
	 * Take an exclusive lock on the bus before calling ->bus_init(), so
	 * that all _locked() helpers can safely be called within this hook.
	 */
	i3c_bus_lock(master->bus, true);
	ret = master->ops->bus_init(master);
	i3c_bus_unlock(master->bus, true);

	return ret;
}

static void i3c_master_cleanup_bus(struct i3c_master_controller *master)
{
	if (master->ops->bus_cleanup) {
		/*
		 * Take an exclusive lock on the bus before calling
		 * ->bus_cleanup(), so that all _locked() helpers can safely be
		 * called within this hook.
		 */
		i3c_bus_lock(master->bus, true);
		master->ops->bus_cleanup(master);
		i3c_bus_unlock(master->bus, true);
	}
}

static void i3c_master_destroy_bus(struct i3c_master_controller *master)
{
	i3c_bus_unregister(master->bus);
	i3c_bus_destroy(master->bus);
}

static int i3c_master_create_bus(struct i3c_master_controller *master)
{
	struct i3c_bus *i3cbus;
	int ret;

	i3cbus = i3c_bus_create(master->parent);
	if (IS_ERR(i3cbus))
		return PTR_ERR(i3cbus);

	i3cbus->cur_master = &master->base;
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
	i3c_bus_destroy(i3cbus);

	return ret;
}

/**
 * i3c_master_register() - register an I3C master
 *
 * @master: master used to send frames on the bus
 * @parent: the parent device (the one that provides this I3C master
 *	    controller)
 * @ops: the master controller operations
 * @secondary: true if you are registering a secondary master. Will return
 *	       -ENOTSUPP if set to true since secondary masters are not yet
 *	       supported.
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

	master->parent = parent;
	master->ops = ops;
	master->secondary = secondary;

	ret = i3c_master_create_bus(master);
	if (ret)
		return ret;

	/*
	 * Before doing any operation on the bus, we need to initialize it.
	 * This operation is highly controller dependent, but it is expected
	 * to do the following operations:
	 * 1/ reset all addresses of all devices on the bus (using RSTDAA CCC
	 *    command)
	 * 2/ start a DAA (Dynamic Address Assignment) procedure
	 * 3/ populate the bus with all I3C devices discovered during DAA using
	 *
	 */
	ret = i3c_master_init_bus(master);
	if (ret)
		goto err_destroy_bus;

	/*
	 * Register a dummy device to represent this master under the I3C bus
	 * in sysfs.
	 */
	ret = device_register(&master->base.dev);
	if (ret)
		goto err_cleanup_bus;

	/* Register all I3C devs that have been added during DAA. */
	ret = i3c_master_register_i3c_devs(master);
	if (ret)
		goto err_unreg_master_dev;

	/*
	 * This is the last step: expose our i3c bus as an i2c adapter so that
	 * i2c devices are exposed through the i2c subsystem.
	 */
	ret = i3c_master_i2c_adapter_init(master);
	if (ret)
		goto err_unreg_i3c_devs;

	return 0;

err_unreg_i3c_devs:
	i3c_master_unregister_i3c_devs(master);

err_unreg_master_dev:
	device_unregister(&master->base.dev);

err_cleanup_bus:
	i3c_master_cleanup_bus(master);

err_destroy_bus:
	i3c_master_destroy_bus(master);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_master_register);

/**
 * i3c_master_unregister() - unregister an I3C master
 *
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

	i3c_master_cleanup_bus(master);

	i3c_master_remove_devs(master);

	i3c_master_destroy_bus(master);

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_master_unregister);
