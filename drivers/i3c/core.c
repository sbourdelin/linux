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

#include <linux/idr.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/slab.h>

#include "internals.h"

static DEFINE_IDR(i3c_bus_idr);
static DEFINE_MUTEX(i3c_core_lock);

void i3c_bus_lock(struct i3c_bus *bus, bool exclusive)
{
	if (exclusive)
		down_write(&bus->lock);
	else
		down_read(&bus->lock);
}

void i3c_bus_unlock(struct i3c_bus *bus, bool exclusive)
{
	if (exclusive)
		up_write(&bus->lock);
	else
		up_read(&bus->lock);
}

static ssize_t bcr_show(struct device *dev,
			struct device_attribute *da,
			char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_bus *bus = i3c_device_get_bus(i3cdev);
	ssize_t ret;

	i3c_bus_lock(bus, false);
	ret = sprintf(buf, "%x\n", i3cdev->info.bcr);
	i3c_bus_unlock(bus, false);

	return ret;
}
static DEVICE_ATTR_RO(bcr);

static ssize_t dcr_show(struct device *dev,
			struct device_attribute *da,
			char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_bus *bus = i3c_device_get_bus(i3cdev);
	ssize_t ret;

	i3c_bus_lock(bus, false);
	ret = sprintf(buf, "%x\n", i3cdev->info.dcr);
	i3c_bus_unlock(bus, false);

	return ret;
}
static DEVICE_ATTR_RO(dcr);

static ssize_t pid_show(struct device *dev,
			struct device_attribute *da,
			char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_bus *bus = i3c_device_get_bus(i3cdev);
	ssize_t ret;

	i3c_bus_lock(bus, false);
	ret = sprintf(buf, "%llx\n", i3cdev->info.pid);
	i3c_bus_unlock(bus, false);

	return ret;
}
static DEVICE_ATTR_RO(pid);

static ssize_t address_show(struct device *dev,
			    struct device_attribute *da,
			    char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_bus *bus = i3c_device_get_bus(i3cdev);
	ssize_t ret;

	i3c_bus_lock(bus, false);
	ret = sprintf(buf, "%02x\n", i3cdev->info.dyn_addr);
	i3c_bus_unlock(bus, false);

	return ret;
}
static DEVICE_ATTR_RO(address);

static const char * const hdrcap_strings[] = {
	"hdr-ddr", "hdr-tsp", "hdr-tsl",
};

static ssize_t hdrcap_show(struct device *dev,
			   struct device_attribute *da,
			   char *buf)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_bus *bus = i3c_device_get_bus(i3cdev);
	unsigned long caps = i3cdev->info.hdr_cap;
	ssize_t offset = 0, ret;
	int mode;

	i3c_bus_lock(bus, false);
	for_each_set_bit(mode, &caps, 8) {
		if (mode >= ARRAY_SIZE(hdrcap_strings))
			break;

		if (!hdrcap_strings[mode])
			continue;

		ret = sprintf(buf + offset, "%s\n", hdrcap_strings[mode]);
		if (ret < 0)
			goto out;

		offset += ret;
	}
	ret = offset;

out:
	i3c_bus_unlock(bus, false);

	return ret;
}
static DEVICE_ATTR_RO(hdrcap);

static struct attribute *i3c_device_attrs[] = {
	&dev_attr_bcr.attr,
	&dev_attr_dcr.attr,
	&dev_attr_pid.attr,
	&dev_attr_address.attr,
	&dev_attr_hdrcap.attr,
	NULL,
};

static const struct attribute_group i3c_device_group = {
	.attrs = i3c_device_attrs,
};

static const struct attribute_group *i3c_device_groups[] = {
	&i3c_device_group,
	NULL,
};

static int i3c_device_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	u16 manuf = I3C_PID_MANUF_ID(i3cdev->info.pid);
	u16 part = I3C_PID_PART_ID(i3cdev->info.pid);
	u16 ext = I3C_PID_EXTRA_INFO(i3cdev->info.pid);

	if (I3C_PID_RND_LOWER_32BITS(i3cdev->info.pid))
		return add_uevent_var(env, "MODALIAS=i3c:dcr%02Xmanuf%04X",
				      i3cdev->info.dcr, manuf);

	return add_uevent_var(env,
			      "MODALIAS=i3c:dcr%02Xmanuf%04Xpart%04xext%04x",
			      i3cdev->info.dcr, manuf, part, ext);
}

const struct device_type i3c_device_type = {
	.groups	= i3c_device_groups,
	.uevent = i3c_device_uevent,
};

static const struct attribute_group *i3c_master_groups[] = {
	&i3c_device_group,
	NULL,
};

const struct device_type i3c_master_type = {
	.groups	= i3c_master_groups,
};

static const char * const i3c_bus_mode_strings[] = {
	[I3C_BUS_MODE_PURE] = "pure",
	[I3C_BUS_MODE_MIXED_FAST] = "mixed-fast",
	[I3C_BUS_MODE_MIXED_SLOW] = "mixed-slow",
};

static ssize_t mode_show(struct device *dev,
			 struct device_attribute *da,
			 char *buf)
{
	struct i3c_bus *i3cbus = container_of(dev, struct i3c_bus, dev);
	ssize_t ret;

	i3c_bus_lock(i3cbus, false);
	if (i3cbus->mode < 0 ||
	    i3cbus->mode > ARRAY_SIZE(i3c_bus_mode_strings) ||
	    !i3c_bus_mode_strings[i3cbus->mode])
		ret = sprintf(buf, "unknown\n");
	else
		ret = sprintf(buf, "%s\n", i3c_bus_mode_strings[i3cbus->mode]);
	i3c_bus_unlock(i3cbus, false);

	return ret;
}
static DEVICE_ATTR_RO(mode);

static ssize_t current_master_show(struct device *dev,
				   struct device_attribute *da,
				   char *buf)
{
	struct i3c_bus *i3cbus = container_of(dev, struct i3c_bus, dev);
	ssize_t ret;

	i3c_bus_lock(i3cbus, false);
	ret = sprintf(buf, "%s\n", dev_name(&i3cbus->cur_master->dev));
	i3c_bus_unlock(i3cbus, false);

	return ret;
}
static DEVICE_ATTR_RO(current_master);

static ssize_t i3c_scl_frequency_show(struct device *dev,
				      struct device_attribute *da,
				      char *buf)
{
	struct i3c_bus *i3cbus = container_of(dev, struct i3c_bus, dev);
	ssize_t ret;

	i3c_bus_lock(i3cbus, false);
	ret = sprintf(buf, "%ld\n", i3cbus->scl_rate.i3c);
	i3c_bus_unlock(i3cbus, false);

	return ret;
}
static DEVICE_ATTR_RO(i3c_scl_frequency);

static ssize_t i2c_scl_frequency_show(struct device *dev,
				      struct device_attribute *da,
				      char *buf)
{
	struct i3c_bus *i3cbus = container_of(dev, struct i3c_bus, dev);
	ssize_t ret;

	i3c_bus_lock(i3cbus, false);
	ret = sprintf(buf, "%ld\n", i3cbus->scl_rate.i2c);
	i3c_bus_unlock(i3cbus, false);

	return ret;
}
static DEVICE_ATTR_RO(i2c_scl_frequency);

static struct attribute *i3c_busdev_attrs[] = {
	&dev_attr_mode.attr,
	&dev_attr_current_master.attr,
	&dev_attr_i3c_scl_frequency.attr,
	&dev_attr_i2c_scl_frequency.attr,
	NULL,
};
ATTRIBUTE_GROUPS(i3c_busdev);

static const struct device_type i3c_busdev_type = {
	.groups	= i3c_busdev_groups,
};

static const struct i3c_device_id *
i3c_device_match_id(struct i3c_device *i3cdev,
		    const struct i3c_device_id *id_table)
{
	const struct i3c_device_id *id;

	/*
	 * The lower 32bits of the provisional ID is just filled with a random
	 * value, try to match using DCR info.
	 */
	if (!I3C_PID_RND_LOWER_32BITS(i3cdev->info.pid)) {
		u16 manuf = I3C_PID_MANUF_ID(i3cdev->info.pid);
		u16 part = I3C_PID_PART_ID(i3cdev->info.pid);
		u16 ext_info = I3C_PID_EXTRA_INFO(i3cdev->info.pid);

		/* First try to match by manufacturer/part ID. */
		for (id = id_table; id->match_flags != 0; id++) {
			if ((id->match_flags & I3C_MATCH_MANUF_AND_PART) !=
			    I3C_MATCH_MANUF_AND_PART)
				continue;

			if (manuf != id->manuf_id || part != id->part_id)
				continue;

			if ((id->match_flags & I3C_MATCH_EXTRA_INFO) &&
			    ext_info != id->extra_info)
				continue;

			return id;
		}
	}

	/* Fallback to DCR match. */
	for (id = id_table; id->match_flags != 0; id++) {
		if ((id->match_flags & I3C_MATCH_DCR) &&
		    id->dcr == i3cdev->info.dcr)
			return id;
	}

	return NULL;
}

static int i3c_device_match(struct device *dev, struct device_driver *drv)
{
	struct i3c_device *i3cdev;
	struct i3c_driver *i3cdrv;

	if (dev->type != &i3c_device_type)
		return 0;

	i3cdev = dev_to_i3cdev(dev);
	i3cdrv = drv_to_i3cdrv(drv);
	if (i3c_device_match_id(i3cdev, i3cdrv->id_table))
		return 1;

	return 0;
}

static int i3c_device_probe(struct device *dev)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_driver *driver = drv_to_i3cdrv(dev->driver);

	return driver->probe(i3cdev);
}

static int i3c_device_remove(struct device *dev)
{
	struct i3c_device *i3cdev = dev_to_i3cdev(dev);
	struct i3c_driver *driver = drv_to_i3cdrv(dev->driver);

	return driver->remove(i3cdev);
}

struct bus_type i3c_bus_type = {
	.name = "i3c",
	.match = i3c_device_match,
	.probe = i3c_device_probe,
	.remove = i3c_device_remove,
};

enum i3c_addr_slot_status i3c_bus_get_addr_slot_status(struct i3c_bus *bus,
						       u16 addr)
{
	int status, bitpos = addr * 2;

	if (addr > I2C_MAX_ADDR)
		return I3C_ADDR_SLOT_RSVD;

	status = bus->addrslots[bitpos / BITS_PER_LONG];
	status >>= bitpos % BITS_PER_LONG;

	return status & I3C_ADDR_SLOT_STATUS_MASK;
}

void i3c_bus_set_addr_slot_status(struct i3c_bus *bus, u16 addr,
				  enum i3c_addr_slot_status status)
{
	int bitpos = addr * 2;
	unsigned long *ptr;

	if (addr > I2C_MAX_ADDR)
		return;

	ptr = bus->addrslots + (bitpos / BITS_PER_LONG);
	*ptr &= ~(I3C_ADDR_SLOT_STATUS_MASK << (bitpos % BITS_PER_LONG));
	*ptr |= status << (bitpos % BITS_PER_LONG);
}

bool i3c_bus_dev_addr_is_avail(struct i3c_bus *bus, u8 addr)
{
	enum i3c_addr_slot_status status;

	status = i3c_bus_get_addr_slot_status(bus, addr);

	return status == I3C_ADDR_SLOT_FREE;
}

int i3c_bus_get_free_addr(struct i3c_bus *bus, u8 start_addr)
{
	enum i3c_addr_slot_status status;
	u8 addr;

	for (addr = start_addr; addr < I3C_MAX_ADDR; addr++) {
		status = i3c_bus_get_addr_slot_status(bus, addr);
		if (status == I3C_ADDR_SLOT_FREE)
			return addr;
	}

	return -ENOMEM;
}

static void i3c_bus_init_addrslots(struct i3c_bus *bus)
{
	int i;

	/* Addresses 0 to 7 are reserved. */
	for (i = 0; i < 8; i++)
		i3c_bus_set_addr_slot_status(bus, i, I3C_ADDR_SLOT_RSVD);

	/*
	 * Reserve broadcast address and all addresses that might collide
	 * with the broadcast address when facing a single bit error.
	 */
	i3c_bus_set_addr_slot_status(bus, I3C_BROADCAST_ADDR,
				     I3C_ADDR_SLOT_RSVD);
	for (i = 0; i < 7; i++)
		i3c_bus_set_addr_slot_status(bus, I3C_BROADCAST_ADDR ^ BIT(i),
					     I3C_ADDR_SLOT_RSVD);
}

void i3c_bus_destroy(struct i3c_bus *bus)
{
	mutex_lock(&i3c_core_lock);
	idr_remove(&i3c_bus_idr, bus->id);
	mutex_unlock(&i3c_core_lock);

	kfree(bus);
}

struct i3c_bus *i3c_bus_create(struct device *parent)
{
	struct i3c_bus *i3cbus;
	int ret;

	i3cbus = kzalloc(sizeof(*i3cbus), GFP_KERNEL);
	if (!i3cbus)
		return ERR_PTR(-ENOMEM);

	init_rwsem(&i3cbus->lock);
	INIT_LIST_HEAD(&i3cbus->devs.i2c);
	INIT_LIST_HEAD(&i3cbus->devs.i3c);
	i3c_bus_init_addrslots(i3cbus);
	i3cbus->mode = I3C_BUS_MODE_PURE;
	i3cbus->dev.parent = parent;
	i3cbus->dev.of_node = parent->of_node;
	i3cbus->dev.bus = &i3c_bus_type;
	i3cbus->dev.type = &i3c_busdev_type;

	mutex_lock(&i3c_core_lock);
	ret = idr_alloc(&i3c_bus_idr, i3cbus, 0, 0, GFP_KERNEL);
	mutex_unlock(&i3c_core_lock);
	if (ret < 0)
		goto err_free_bus;

	i3cbus->id = ret;

	return i3cbus;

err_free_bus:
	kfree(i3cbus);

	return ERR_PTR(ret);
}

void i3c_bus_unregister(struct i3c_bus *bus)
{
	device_unregister(&bus->dev);
}

int i3c_bus_register(struct i3c_bus *i3cbus)
{
	struct i2c_device *i2cdev;

	i3c_bus_for_each_i2cdev(i3cbus, i2cdev) {
		switch (i2cdev->lvr & I3C_LVR_I2C_INDEX_MASK) {
		case I3C_LVR_I2C_INDEX(0):
			if (i3cbus->mode < I3C_BUS_MODE_MIXED_FAST)
				i3cbus->mode = I3C_BUS_MODE_MIXED_FAST;
			break;

		case I3C_LVR_I2C_INDEX(1):
		case I3C_LVR_I2C_INDEX(2):
			if (i3cbus->mode < I3C_BUS_MODE_MIXED_SLOW)
				i3cbus->mode = I3C_BUS_MODE_MIXED_SLOW;
			break;

		default:
			return -EINVAL;
		}
	}

	if (!i3cbus->scl_rate.i3c)
		i3cbus->scl_rate.i3c = I3C_BUS_TYP_I3C_SCL_RATE;

	if (!i3cbus->scl_rate.i2c) {
		if (i3cbus->mode == I3C_BUS_MODE_MIXED_SLOW)
			i3cbus->scl_rate.i2c = I3C_BUS_I2C_FM_SCL_RATE;
		else
			i3cbus->scl_rate.i2c = I3C_BUS_I2C_FM_PLUS_SCL_RATE;
	}

	/*
	 * I3C/I2C frequency may have been overridden, check that user-provided
	 * values are not exceeding max possible frequency.
	 */
	if (i3cbus->scl_rate.i3c > I3C_BUS_MAX_I3C_SCL_RATE ||
	    i3cbus->scl_rate.i2c > I3C_BUS_I2C_FM_PLUS_SCL_RATE) {
		return -EINVAL;
	}

	dev_set_name(&i3cbus->dev, "i3c-%d", i3cbus->id);

	return device_register(&i3cbus->dev);
}

static int __init i3c_init(void)
{
	return bus_register(&i3c_bus_type);
}
subsys_initcall(i3c_init);

static void __exit i3c_exit(void)
{
	bus_unregister(&i3c_bus_type);
}
module_exit(i3c_exit);
