/*
 * Mediated device Core Driver
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

#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/uuid.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/sysfs.h>
#include <linux/mdev.h>

#include "mdev_private.h"

#define DRIVER_VERSION		"0.2"
#define DRIVER_AUTHOR		"NVIDIA Corporation"
#define DRIVER_DESC		"Mediated Device Core Driver"


static int __find_mdev_device(struct device *dev, void *data)
{
	struct mdev_device *mdev = dev_to_mdev(dev);

	return (uuid_le_cmp(mdev->uuid, *(uuid_le *)data) == 0);
}

static struct mdev_device *find_mdev_device(struct mdev_host *host,
					    uuid_le uuid)
{
	struct device *dev;

	dev = device_find_child(&host->dev, &uuid, __find_mdev_device);
	if (!dev)
		return NULL;

	return dev_to_mdev(dev);
}

static int mdev_device_create_ops(struct mdev_device *mdev, char *mdev_params)
{
	struct mdev_host *host = dev_to_host(mdev->dev.parent);

	return host->ops->create(mdev, mdev_params);
}

static void mdev_device_destroy_ops(struct mdev_device *mdev)
{
	struct mdev_host *host = dev_to_host(mdev->dev.parent);

	host->ops->destroy(mdev);
}

/*
 * mdev_register_host_device : register a mdev host device
 * @dev: device structure of the physical device under which the created
 *       host device will be.
 * @ops: Parent device operation structure to be registered.
 *
 * Register a mdev host device as the mediator of mdev devices.
 * Returns the pointer of mdev host device structure for success, NULL
 * for errors.
 */
struct mdev_host *mdev_register_host_device(struct device *pdev,
				const struct mdev_host_ops *ops)
{
	int rc = 0;
	struct mdev_host *host;

	if (!pdev || !ops) {
		dev_warn(pdev, "dev or ops is NULL\n");
		return NULL;
	}

	/* check for mandatory ops */
	if (!ops->create || !ops->destroy) {
		dev_warn(pdev, "create and destroy methods are necessary\n");
		return NULL;
	}

	host = kzalloc(sizeof(*host), GFP_KERNEL);
	if (!host)
		return NULL;

	host->dev.parent = pdev;
	host->ops = ops;
	dev_set_name(&host->dev, "mdev-host");

	rc = device_register(&host->dev);
	if (rc)
		goto register_error;

	rc = mdev_create_sysfs_files(&host->dev);
	if (rc)
		goto add_sysfs_error;

	rc = sysfs_create_groups(&host->dev.kobj, ops->hdev_attr_groups);
	if (rc)
		goto add_group_error;

	dev_info(&host->dev, "mdev host device registered\n");
	return host;

add_group_error:
	mdev_remove_sysfs_files(&host->dev);

add_sysfs_error:
	device_unregister(&host->dev);

register_error:
	kfree(host);
	return NULL;
}
EXPORT_SYMBOL(mdev_register_host_device);

static int __mdev_device_destroy(struct device *dev, void *data)
{
	struct mdev_device *mdev = dev_to_mdev(dev);

	mdev_device_destroy_ops(mdev);
	device_unregister(&mdev->dev);

	return 0;
}

/*
 * mdev_unregister_host_device : unregister a mdev host device
 * @host: the mdev host device structure
 *
 * Unregister a mdev host device as the mediator
 */
void mdev_unregister_host_device(struct mdev_host *host)
{
	if (!host)
		return;

	dev_info(&host->dev, "mdev host device unregistered\n");

	mdev_remove_sysfs_files(&host->dev);
	sysfs_remove_groups(&host->dev.kobj, host->ops->hdev_attr_groups);
	device_for_each_child(&host->dev, NULL,  __mdev_device_destroy);
	device_unregister(&host->dev);
}
EXPORT_SYMBOL(mdev_unregister_host_device);

int mdev_device_create(struct device *dev, uuid_le uuid, char *mdev_params)
{
	int ret;
	struct mdev_device *mdev;
	struct mdev_host *host = dev_to_host(dev);

	/* Check for duplicate */
	mdev = find_mdev_device(host, uuid);
	if (mdev) {
		ret = -EEXIST;
		goto create_err;
	}

	mdev = kzalloc(sizeof(*mdev), GFP_KERNEL);
	if (!mdev) {
		ret = -ENOMEM;
		goto create_err;
	}

	memcpy(&mdev->uuid, &uuid, sizeof(uuid_le));

	mdev->dev.parent = dev;
	mdev->dev.bus = &mdev_bus_type;
	mdev->dev.groups = host->ops->mdev_attr_groups;
	dev_set_name(&mdev->dev, "%pUl", uuid.b);

	ret = device_register(&mdev->dev);
	if (ret) {
		put_device(&mdev->dev);
		goto create_err;
	}

	ret = mdev_device_create_ops(mdev, mdev_params);
	if (ret)
		goto create_failed;

	dev_dbg(&mdev->dev, "MDEV: created\n");

	return ret;

create_failed:
	device_unregister(&mdev->dev);

create_err:
	return ret;
}

int mdev_device_destroy(struct device *dev, uuid_le uuid)
{
	struct mdev_device *mdev;
	struct mdev_host *host = dev_to_host(dev);

	mdev = find_mdev_device(host, uuid);
	if (!mdev)
		return -ENODEV;

	return __mdev_device_destroy(&mdev->dev, NULL);
}

void mdev_device_supported_config(struct device *dev, char *str)
{
	struct mdev_host *host = dev_to_host(dev);

	if (host->ops->supported_config)
		host->ops->supported_config(&host->dev, str);
}

static int __init mdev_init(void)
{
	int ret;

	ret = mdev_bus_register();
	if (ret)
		pr_err("failed to register mdev bus: %d\n", ret);

	return ret;
}

static void __exit mdev_exit(void)
{
	mdev_bus_unregister();
}

module_init(mdev_init)
module_exit(mdev_exit)

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
