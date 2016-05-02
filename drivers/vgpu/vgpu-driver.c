/*
 * VGPU driver
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/sysfs.h>
#include <linux/ctype.h>
#include <linux/vgpu.h>

#include "vgpu_private.h"

static int vgpu_device_attach_iommu(struct vgpu_device *vgpu_dev)
{
        int retval = 0;
        struct iommu_group *group = NULL;

        group = iommu_group_alloc();
        if (IS_ERR(group)) {
                printk(KERN_ERR "VGPU: failed to allocate group!\n");
                return PTR_ERR(group);
        }

        retval = iommu_group_add_device(group, &vgpu_dev->dev);
        if (retval) {
                printk(KERN_ERR "VGPU: failed to add dev to group!\n");
                iommu_group_put(group);
                return retval;
        }

        vgpu_dev->group = group;

        printk(KERN_INFO "VGPU: group_id = %d \n", iommu_group_id(group));
        return retval;
}

static void vgpu_device_detach_iommu(struct vgpu_device *vgpu_dev)
{
        iommu_group_put(vgpu_dev->dev.iommu_group);
        iommu_group_remove_device(&vgpu_dev->dev);
        printk(KERN_INFO "VGPU: detaching iommu \n");
}

static int vgpu_device_probe(struct device *dev)
{
	struct vgpu_driver *drv = to_vgpu_driver(dev->driver);
	struct vgpu_device *vgpu_dev = to_vgpu_device(dev);
	int status = 0;

	status = vgpu_device_attach_iommu(vgpu_dev);
	if (status) {
		printk(KERN_ERR "Failed to attach IOMMU\n");
		return status;
	}

	if (drv && drv->probe) {
		status = drv->probe(dev);
	}

	return status;
}

static int vgpu_device_remove(struct device *dev)
{
	struct vgpu_driver *drv = to_vgpu_driver(dev->driver);
	struct vgpu_device *vgpu_dev = to_vgpu_device(dev);
	int status = 0;

	if (drv && drv->remove) {
		drv->remove(dev);
	}

	vgpu_device_detach_iommu(vgpu_dev);

	return status;
}

struct bus_type vgpu_bus_type = {
	.name		= "vgpu",
	.probe		= vgpu_device_probe,
	.remove		= vgpu_device_remove,
};
EXPORT_SYMBOL_GPL(vgpu_bus_type);

/**
 * vgpu_register_driver - register a new vGPU driver
 * @drv: the driver to register
 * @owner: owner module of driver ro register
 *
 * Returns a negative value on error, otherwise 0.
 */
int vgpu_register_driver(struct vgpu_driver *drv, struct module *owner)
{
	/* initialize common driver fields */
	drv->driver.name = drv->name;
	drv->driver.bus = &vgpu_bus_type;
	drv->driver.owner = owner;

	/* register with core */
	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(vgpu_register_driver);

/**
 * vgpu_unregister_driver - unregister vGPU driver
 * @drv: the driver to unregister
 *
 */
void vgpu_unregister_driver(struct vgpu_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(vgpu_unregister_driver);

int vgpu_bus_register(void)
{
	return bus_register(&vgpu_bus_type);
}

void vgpu_bus_unregister(void)
{
	bus_unregister(&vgpu_bus_type);
}
