// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Cadence Design Systems Inc.
 *
 * Author: Boris Brezillon <boris.brezillon@bootlin.com>
 */

#include <linux/atomic.h>
#include <linux/bug.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/mutex.h>
#include <linux/slab.h>

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
	int ret, i;

	if (nxfers < 1)
		return 0;

	master = i3c_device_get_master(dev);
	if (!master || !xfers)
		return -EINVAL;

	if (!master->ops->priv_xfers)
		return -ENOTSUPP;

	for (i = 0; i < nxfers; i++) {
		if (!xfers[i].len || !xfers[i].data.in)
			return -EINVAL;
	}

	i3c_bus_normaluse_lock(master->bus);
	ret = master->ops->priv_xfers(dev, xfers, nxfers);
	i3c_bus_normaluse_unlock(master->bus);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_do_priv_xfers);

/**
 * i3c_device_get_info() - get I3C device information
 *
 * @dev: device we want information on
 * @info: the information object to fill in
 *
 * Retrieve I3C dev info.
 */
void i3c_device_get_info(struct i3c_device *dev,
			 struct i3c_device_info *info)
{
	if (info)
		*info = dev->info;
}
EXPORT_SYMBOL_GPL(i3c_device_get_info);

/**
 * i3c_device_disable_ibi() - Disable IBIs coming from a specific device
 * @dev: device on which IBIs should be disabled
 *
 * This function disable IBIs coming from a specific device and wait for
 * all pending IBIs to be processed.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_disable_ibi(struct i3c_device *dev)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);
	int ret;

	i3c_bus_normaluse_lock(master->bus);
	mutex_lock(&dev->ibi_lock);
	if (!dev->ibi) {
		ret = -EINVAL;
		goto out;
	}

	ret = master->ops->disable_ibi(dev);
	if (ret)
		goto out;

	reinit_completion(&dev->ibi->all_ibis_handled);
	if (atomic_read(&dev->ibi->pending_ibis))
		wait_for_completion(&dev->ibi->all_ibis_handled);

	dev->ibi->enabled = false;

out:
	mutex_unlock(&dev->ibi_lock);
	i3c_bus_normaluse_unlock(master->bus);

	return 0;
}
EXPORT_SYMBOL_GPL(i3c_device_disable_ibi);

/**
 * i3c_device_enable_ibi() - Enable IBIs coming from a specific device
 * @dev: device on which IBIs should be enabled
 *
 * This function enable IBIs coming from a specific device and wait for
 * all pending IBIs to be processed. This should be called on a device
 * where i3c_device_request_ibi() has succeeded.
 *
 * Note that IBIs from this device might be received before this function
 * returns to its caller.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_enable_ibi(struct i3c_device *dev)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);
	int ret;

	i3c_bus_normaluse_lock(master->bus);
	mutex_lock(&dev->ibi_lock);
	if (!dev->ibi) {
		ret = -EINVAL;
		goto out;
	}

	ret = master->ops->enable_ibi(dev);
	if (!ret)
		dev->ibi->enabled = true;

out:
	mutex_unlock(&dev->ibi_lock);
	i3c_bus_normaluse_unlock(master->bus);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_enable_ibi);

/**
 * i3c_device_request_ibi() - Request an IBI
 * @dev: device for which we should enable IBIs
 * @req: setup requested for this IBI
 *
 * This function is responsible for pre-allocating all resources needed to
 * process IBIs coming from @dev. When this function returns, the IBI is not
 * enabled until i3c_device_enable_ibi() is called.
 *
 * Return: 0 in case of success, a negative error core otherwise.
 */
int i3c_device_request_ibi(struct i3c_device *dev,
			   const struct i3c_ibi_setup *req)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);
	struct i3c_device_ibi_info *ibi;
	int ret;

	if (!master->ops->request_ibi)
		return -ENOTSUPP;

	if (!req->handler || !req->num_slots)
		return -EINVAL;

	i3c_bus_normaluse_lock(master->bus);
	mutex_lock(&dev->ibi_lock);
	if (dev->ibi) {
		ret = -EBUSY;
		goto out;
	}

	ibi = kzalloc(sizeof(*ibi), GFP_KERNEL);
	if (!ibi) {
		ret = -ENOMEM;
		goto out;
	}

	atomic_set(&ibi->pending_ibis, 0);
	init_completion(&ibi->all_ibis_handled);
	ibi->handler = req->handler;
	ibi->max_payload_len = req->max_payload_len;

	dev->ibi = ibi;
	ret = master->ops->request_ibi(dev, req);
	if (ret) {
		kfree(ibi);
		dev->ibi = NULL;
	}

out:
	mutex_unlock(&dev->ibi_lock);
	i3c_bus_normaluse_unlock(master->bus);

	return ret;
}
EXPORT_SYMBOL_GPL(i3c_device_request_ibi);

/**
 * i3c_device_free_ibi() - Free all resources needed for IBI handling
 * @dev: device on which you want to release IBI resources
 *
 * This function is responsible for de-allocating resources previously
 * allocated by i3c_device_request_ibi(). It should be called after disabling
 * IBIs with i3c_device_disable_ibi().
 */
void i3c_device_free_ibi(struct i3c_device *dev)
{
	struct i3c_master_controller *master = i3c_device_get_master(dev);

	i3c_bus_normaluse_lock(master->bus);
	mutex_lock(&dev->ibi_lock);
	if (!dev->ibi)
		goto out;

	if (WARN_ON(dev->ibi->enabled))
		BUG_ON(i3c_device_disable_ibi(dev));

	master->ops->free_ibi(dev);
	kfree(dev->ibi);
	dev->ibi = NULL;

out:
	mutex_unlock(&dev->ibi_lock);
	i3c_bus_normaluse_unlock(master->bus);
}
EXPORT_SYMBOL_GPL(i3c_device_free_ibi);

/**
 * i3cdev_to_dev() - Returns the device embedded in @i3cdev
 * @i3cdev: I3C device
 *
 * Return: a pointer to a device object.
 */
struct device *i3cdev_to_dev(struct i3c_device *i3cdev)
{
	return &i3cdev->dev;
}
EXPORT_SYMBOL_GPL(i3cdev_to_dev);

/**
 * dev_to_i3cdev() - Returns the I3C device containing @dev
 * @dev: device object
 *
 * Return: a pointer to an I3C device object.
 */
struct i3c_device *dev_to_i3cdev(struct device *dev)
{
	return container_of(dev, struct i3c_device, dev);
}
EXPORT_SYMBOL_GPL(dev_to_i3cdev);

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
