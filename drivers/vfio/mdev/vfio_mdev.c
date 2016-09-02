/*
 * VFIO Bus driver for Mediated device
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * Copyright (c) 2016 Intel Corporation.
 * Author:
 *	Xiao Guangrong <guangrong.xiao@linux.intel.com>
 *	Jike Song <jike.song@intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/mdev.h>

#include "mdev_private.h"

#define DRIVER_VERSION  "0.2"
#define DRIVER_AUTHOR   "NVIDIA Corporation"
#define DRIVER_DESC     "VFIO Bus driver for Mediated device"

struct vfio_mdev {
	struct iommu_group *group;
	struct mdev_device *mdev;
};

static int vfio_mdev_open(void *device_data)
{
	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	return 0;
}

static void vfio_mdev_close(void *device_data)
{
	module_put(THIS_MODULE);
}

static long vfio_mdev_unlocked_ioctl(void *device_data,
				     unsigned int cmd, unsigned long arg)
{
	struct vfio_mdev *vmdev = device_data;
	struct mdev_device *mdev = vmdev->mdev;
	struct mdev_host *host = dev_to_host(mdev->dev.parent);

	if (host->ops->ioctl)
		return host->ops->ioctl(mdev, cmd, arg);

	return -ENODEV;
}

static ssize_t vfio_mdev_read(void *device_data, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct vfio_mdev *vmdev = device_data;
	struct mdev_device *mdev = vmdev->mdev;
	struct mdev_host *host = dev_to_host(mdev->dev.parent);

	if (host->ops->read)
		return host->ops->read(mdev, buf, count, ppos);

	return -ENODEV;
}

static ssize_t vfio_mdev_write(void *device_data, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct vfio_mdev *vmdev = device_data;
	struct mdev_device *mdev = vmdev->mdev;
	struct mdev_host *host = dev_to_host(mdev->dev.parent);

	if (host->ops->write)
		return host->ops->write(mdev, buf, count, ppos);

	return -ENODEV;
}

static int vfio_mdev_mmap(void *device_data, struct vm_area_struct *vma)
{
	struct vfio_mdev *vmdev = device_data;
	struct mdev_device *mdev = vmdev->mdev;
	struct mdev_host *host = dev_to_host(mdev->dev.parent);

	if (host->ops->mmap)
		return host->ops->mmap(mdev, vma);

	return -ENODEV;
}

static const struct vfio_device_ops vfio_mdev_dev_ops = {
	.name		= "vfio-mdev",
	.open		= vfio_mdev_open,
	.release	= vfio_mdev_close,
	.ioctl		= vfio_mdev_unlocked_ioctl,
	.read		= vfio_mdev_read,
	.write		= vfio_mdev_write,
	.mmap		= vfio_mdev_mmap,
};

static int vfio_mdev_probe(struct device *dev)
{
	struct vfio_mdev *vmdev;
	struct mdev_device *mdev = dev_to_mdev(dev);
	int ret;

	vmdev = kzalloc(sizeof(*vmdev), GFP_KERNEL);
	if (IS_ERR(vmdev))
		return PTR_ERR(vmdev);

	vmdev->mdev = mdev;
	vmdev->group = mdev->group;

	ret = vfio_add_group_dev(dev, &vfio_mdev_dev_ops, vmdev);
	if (ret)
		kfree(vmdev);

	return ret;
}

static void vfio_mdev_remove(struct device *dev)
{
	struct vfio_mdev *vmdev;

	vmdev = vfio_del_group_dev(dev);
	kfree(vmdev);
}

static int vfio_mdev_online(struct device *dev)
{
	struct mdev_device *mdev = dev_to_mdev(dev);
	struct mdev_host *host = dev_to_host(mdev->dev.parent);

	if (host->ops->start)
		return host->ops->start(mdev);

	return -ENOTSUPP;
}

static int vfio_mdev_offline(struct device *dev)
{
	struct mdev_device *mdev = dev_to_mdev(dev);
	struct mdev_host *host = dev_to_host(mdev->dev.parent);

	if (host->ops->stop)
		return host->ops->stop(mdev);

	return -ENOTSUPP;
}

static struct mdev_driver vfio_mdev_driver = {
	.name		= "vfio_mdev",
	.probe		= vfio_mdev_probe,
	.remove		= vfio_mdev_remove,
	.online		= vfio_mdev_online,
	.offline	= vfio_mdev_offline,
};

static int __init vfio_mdev_init(void)
{
	return mdev_register_driver(&vfio_mdev_driver, THIS_MODULE);
}

static void __exit vfio_mdev_exit(void)
{
	mdev_unregister_driver(&vfio_mdev_driver);
}

module_init(vfio_mdev_init)
module_exit(vfio_mdev_exit)

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
