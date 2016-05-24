/*
 * MDEV driver
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/device.h>
#include <linux/iommu.h>
#include <linux/mdev.h>

#include "mdev_private.h"

static int mdevice_attach_iommu(struct mdev_device *mdevice)
{
	int retval = 0;
	struct iommu_group *group = NULL;

	group = iommu_group_alloc();
	if (IS_ERR(group)) {
		dev_err(&mdevice->dev, "MDEV: failed to allocate group!\n");
		return PTR_ERR(group);
	}

	retval = iommu_group_add_device(group, &mdevice->dev);
	if (retval) {
		dev_err(&mdevice->dev, "MDEV: failed to add dev to group!\n");
		goto attach_fail;
	}

	mdevice->group = group;

	dev_info(&mdevice->dev, "MDEV: group_id = %d\n",
				 iommu_group_id(group));
attach_fail:
	iommu_group_put(group);
	return retval;
}

static void mdevice_detach_iommu(struct mdev_device *mdevice)
{
	iommu_group_remove_device(&mdevice->dev);
	dev_info(&mdevice->dev, "MDEV: detaching iommu\n");
}

static int mdevice_probe(struct device *dev)
{
	struct mdev_driver *drv = to_mdev_driver(dev->driver);
	struct mdev_device *mdevice = to_mdev_device(dev);
	int status = 0;

	status = mdevice_attach_iommu(mdevice);
	if (status) {
		dev_err(dev, "Failed to attach IOMMU\n");
		return status;
	}

	if (drv && drv->probe)
		status = drv->probe(dev);

	return status;
}

static int mdevice_remove(struct device *dev)
{
	struct mdev_driver *drv = to_mdev_driver(dev->driver);
	struct mdev_device *mdevice = to_mdev_device(dev);

	if (drv && drv->remove)
		drv->remove(dev);

	mdevice_detach_iommu(mdevice);

	return 0;
}

static int mdevice_match(struct device *dev, struct device_driver *drv)
{
	int ret = 0;
	struct mdev_driver *mdrv = to_mdev_driver(drv);

	if (mdrv && mdrv->match)
		ret = mdrv->match(dev);

	return ret;
}

struct bus_type mdev_bus_type = {
	.name		= "mdev",
	.match		= mdevice_match,
	.probe		= mdevice_probe,
	.remove		= mdevice_remove,
};
EXPORT_SYMBOL_GPL(mdev_bus_type);

/**
 * mdev_register_driver - register a new MDEV driver
 * @drv: the driver to register
 * @owner: owner module of driver ro register
 *
 * Returns a negative value on error, otherwise 0.
 */
int mdev_register_driver(struct mdev_driver *drv, struct module *owner)
{
	/* initialize common driver fields */
	drv->driver.name = drv->name;
	drv->driver.bus = &mdev_bus_type;
	drv->driver.owner = owner;

	/* register with core */
	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(mdev_register_driver);

/**
 * mdev_unregister_driver - unregister MDEV driver
 * @drv: the driver to unregister
 *
 */
void mdev_unregister_driver(struct mdev_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(mdev_unregister_driver);

int mdev_bus_register(void)
{
	return bus_register(&mdev_bus_type);
}

void mdev_bus_unregister(void)
{
	bus_unregister(&mdev_bus_type);
}
