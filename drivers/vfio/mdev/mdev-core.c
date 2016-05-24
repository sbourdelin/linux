/*
 * Mediated device Core Driver
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
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/uuid.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/sysfs.h>
#include <linux/mdev.h>

#include "mdev_private.h"

#define DRIVER_VERSION		"0.1"
#define DRIVER_AUTHOR		"NVIDIA Corporation"
#define DRIVER_DESC		"Mediated device Core Driver"

#define MDEV_CLASS_NAME		"mdev"

/*
 * Global Structures
 */

static struct devices_list {
	struct list_head    dev_list;
	struct mutex        list_lock;
} mdevices, phy_devices;

/*
 * Functions
 */

static int mdev_add_attribute_group(struct device *dev,
				    const struct attribute_group **groups)
{
	return sysfs_create_groups(&dev->kobj, groups);
}

static void mdev_remove_attribute_group(struct device *dev,
					const struct attribute_group **groups)
{
	sysfs_remove_groups(&dev->kobj, groups);
}

static struct mdev_device *find_mdev_device(uuid_le uuid, int instance)
{
	struct mdev_device *vdev = NULL, *v;

	mutex_lock(&mdevices.list_lock);
	list_for_each_entry(v, &mdevices.dev_list, next) {
		if ((uuid_le_cmp(v->uuid, uuid) == 0) &&
		    (v->instance == instance)) {
			vdev = v;
			break;
		}
	}
	mutex_unlock(&mdevices.list_lock);
	return vdev;
}

static struct mdev_device *find_next_mdev_device(struct phy_device *phy_dev)
{
	struct mdev_device *mdev = NULL, *p;

	mutex_lock(&mdevices.list_lock);
	list_for_each_entry(p, &mdevices.dev_list, next) {
		if (p->phy_dev == phy_dev) {
			mdev = p;
			break;
		}
	}
	mutex_unlock(&mdevices.list_lock);
	return mdev;
}

static struct phy_device *find_physical_device(struct device *dev)
{
	struct phy_device *pdev = NULL, *p;

	mutex_lock(&phy_devices.list_lock);
	list_for_each_entry(p, &phy_devices.dev_list, next) {
		if (p->dev == dev) {
			pdev = p;
			break;
		}
	}
	mutex_unlock(&phy_devices.list_lock);
	return pdev;
}

static void mdev_destroy_device(struct mdev_device *mdevice)
{
	struct phy_device *phy_dev = mdevice->phy_dev;

	if (phy_dev) {
		mutex_lock(&phy_devices.list_lock);

		/*
		* If vendor driver doesn't return success that means vendor
		* driver doesn't support hot-unplug
		*/
		if (phy_dev->ops->destroy) {
			if (phy_dev->ops->destroy(phy_dev->dev, mdevice->uuid,
						  mdevice->instance)) {
				mutex_unlock(&phy_devices.list_lock);
				return;
			}
		}

		mdev_remove_attribute_group(&mdevice->dev,
					    phy_dev->ops->mdev_attr_groups);
		mdevice->phy_dev = NULL;
		mutex_unlock(&phy_devices.list_lock);
	}

	mdev_put_device(mdevice);
	device_unregister(&mdevice->dev);
}

/*
 * Find mediated device from given iommu_group and increment refcount of
 * mediated device. Caller should call mdev_put_device() when the use of
 * mdev_device is done.
 */
struct mdev_device *mdev_get_device_by_group(struct iommu_group *group)
{
	struct mdev_device *mdev = NULL, *p;

	mutex_lock(&mdevices.list_lock);
	list_for_each_entry(p, &mdevices.dev_list, next) {
		if (!p->group)
			continue;

		if (iommu_group_id(p->group) == iommu_group_id(group)) {
			mdev = mdev_get_device(p);
			break;
		}
	}
	mutex_unlock(&mdevices.list_lock);
	return mdev;
}
EXPORT_SYMBOL_GPL(mdev_get_device_by_group);

/*
 * mdev_register_device : Register a device
 * @dev: device structure representing physical device.
 * @phy_device_ops: Physical device operation structure to be registered.
 *
 * Add device to list of registered physical devices.
 * Returns a negative value on error, otherwise 0.
 */
int mdev_register_device(struct device *dev, const struct phy_device_ops *ops)
{
	int ret = 0;
	struct phy_device *phy_dev, *pdev;

	if (!dev || !ops)
		return -EINVAL;

	/* Check for duplicate */
	pdev = find_physical_device(dev);
	if (pdev)
		return -EEXIST;

	phy_dev = kzalloc(sizeof(*phy_dev), GFP_KERNEL);
	if (!phy_dev)
		return -ENOMEM;

	phy_dev->dev = dev;
	phy_dev->ops = ops;

	mutex_lock(&phy_devices.list_lock);
	ret = mdev_create_sysfs_files(dev);
	if (ret)
		goto add_sysfs_error;

	ret = mdev_add_attribute_group(dev, ops->dev_attr_groups);
	if (ret)
		goto add_group_error;

	list_add(&phy_dev->next, &phy_devices.dev_list);
	dev_info(dev, "MDEV: Registered\n");
	mutex_unlock(&phy_devices.list_lock);

	return 0;

add_group_error:
	mdev_remove_sysfs_files(dev);
add_sysfs_error:
	mutex_unlock(&phy_devices.list_lock);
	kfree(phy_dev);
	return ret;
}
EXPORT_SYMBOL(mdev_register_device);

/*
 * mdev_unregister_device : Unregister a physical device
 * @dev: device structure representing physical device.
 *
 * Remove device from list of registered physical devices. Gives a change to
 * free existing mediated devices for the given physical device.
 */

void mdev_unregister_device(struct device *dev)
{
	struct phy_device *phy_dev;
	struct mdev_device *vdev = NULL;

	phy_dev = find_physical_device(dev);

	if (!phy_dev)
		return;

	dev_info(dev, "MDEV: Unregistering\n");

	while ((vdev = find_next_mdev_device(phy_dev)))
		mdev_destroy_device(vdev);

	mutex_lock(&phy_devices.list_lock);
	list_del(&phy_dev->next);
	mutex_unlock(&phy_devices.list_lock);

	mdev_remove_attribute_group(dev,
				    phy_dev->ops->dev_attr_groups);

	mdev_remove_sysfs_files(dev);
	kfree(phy_dev);
}
EXPORT_SYMBOL(mdev_unregister_device);

/*
 * Functions required for mdev-sysfs
 */

static struct mdev_device *mdev_device_alloc(uuid_le uuid, int instance)
{
	struct mdev_device *mdevice = NULL;

	mdevice = kzalloc(sizeof(*mdevice), GFP_KERNEL);
	if (!mdevice)
		return ERR_PTR(-ENOMEM);

	kref_init(&mdevice->kref);
	memcpy(&mdevice->uuid, &uuid, sizeof(uuid_le));
	mdevice->instance = instance;
	mutex_init(&mdevice->ops_lock);

	return mdevice;
}

static void mdev_device_release(struct device *dev)
{
	struct mdev_device *mdevice = to_mdev_device(dev);

	if (!mdevice)
		return;

	dev_info(&mdevice->dev, "MDEV: destroying\n");

	mutex_lock(&mdevices.list_lock);
	list_del(&mdevice->next);
	mutex_unlock(&mdevices.list_lock);

	kfree(mdevice);
}

int create_mdev_device(struct device *dev, uuid_le uuid, uint32_t instance,
		       char *mdev_params)
{
	int retval = 0;
	struct mdev_device *mdevice = NULL;
	struct phy_device *phy_dev;

	phy_dev = find_physical_device(dev);
	if (!phy_dev)
		return -EINVAL;

	mdevice = mdev_device_alloc(uuid, instance);
	if (IS_ERR(mdevice)) {
		retval = PTR_ERR(mdevice);
		return retval;
	}

	mdevice->dev.parent  = dev;
	mdevice->dev.bus     = &mdev_bus_type;
	mdevice->dev.release = mdev_device_release;
	dev_set_name(&mdevice->dev, "%pUb-%d", uuid.b, instance);

	mutex_lock(&mdevices.list_lock);
	list_add(&mdevice->next, &mdevices.dev_list);
	mutex_unlock(&mdevices.list_lock);

	retval = device_register(&mdevice->dev);
	if (retval) {
		mdev_put_device(mdevice);
		return retval;
	}

	mutex_lock(&phy_devices.list_lock);
	if (phy_dev->ops->create) {
		retval = phy_dev->ops->create(dev, mdevice->uuid,
					      instance, mdev_params);
		if (retval)
			goto create_failed;
	}

	retval = mdev_add_attribute_group(&mdevice->dev,
					  phy_dev->ops->mdev_attr_groups);
	if (retval)
		goto create_failed;

	mdevice->phy_dev = phy_dev;
	mutex_unlock(&phy_devices.list_lock);
	mdev_get_device(mdevice);
	dev_info(&mdevice->dev, "MDEV: created\n");

	return retval;

create_failed:
	mutex_unlock(&phy_devices.list_lock);
	device_unregister(&mdevice->dev);
	return retval;
}

int destroy_mdev_device(uuid_le uuid, uint32_t instance)
{
	struct mdev_device *vdev;

	vdev = find_mdev_device(uuid, instance);

	if (!vdev)
		return -EINVAL;

	mdev_destroy_device(vdev);
	return 0;
}

void get_mdev_supported_types(struct device *dev, char *str)
{
	struct phy_device *phy_dev;

	phy_dev = find_physical_device(dev);

	if (phy_dev) {
		mutex_lock(&phy_devices.list_lock);
		if (phy_dev->ops->supported_config)
			phy_dev->ops->supported_config(phy_dev->dev, str);
		mutex_unlock(&phy_devices.list_lock);
	}
}

int mdev_start_callback(uuid_le uuid, uint32_t instance)
{
	int ret = 0;
	struct mdev_device *mdevice;
	struct phy_device *phy_dev;

	mdevice = find_mdev_device(uuid, instance);

	if (!mdevice)
		return -EINVAL;

	phy_dev = mdevice->phy_dev;

	mutex_lock(&phy_devices.list_lock);
	if (phy_dev->ops->start)
		ret = phy_dev->ops->start(mdevice->uuid);
	mutex_unlock(&phy_devices.list_lock);

	if (ret < 0)
		pr_err("mdev_start failed  %d\n", ret);
	else
		kobject_uevent(&mdevice->dev.kobj, KOBJ_ONLINE);

	return ret;
}

int mdev_shutdown_callback(uuid_le uuid, uint32_t instance)
{
	int ret = 0;
	struct mdev_device *mdevice;
	struct phy_device *phy_dev;

	mdevice = find_mdev_device(uuid, instance);

	if (!mdevice)
		return -EINVAL;

	phy_dev = mdevice->phy_dev;

	mutex_lock(&phy_devices.list_lock);
	if (phy_dev->ops->shutdown)
		ret = phy_dev->ops->shutdown(mdevice->uuid);
	mutex_unlock(&phy_devices.list_lock);

	if (ret < 0)
		pr_err("mdev_shutdown failed %d\n", ret);
	else
		kobject_uevent(&mdevice->dev.kobj, KOBJ_OFFLINE);

	return ret;
}

static struct class mdev_class = {
	.name		= MDEV_CLASS_NAME,
	.owner		= THIS_MODULE,
	.class_attrs	= mdev_class_attrs,
};

static int __init mdev_init(void)
{
	int rc = 0;

	mutex_init(&mdevices.list_lock);
	INIT_LIST_HEAD(&mdevices.dev_list);
	mutex_init(&phy_devices.list_lock);
	INIT_LIST_HEAD(&phy_devices.dev_list);

	rc = class_register(&mdev_class);
	if (rc < 0) {
		pr_err("Failed to register mdev class\n");
		return rc;
	}

	rc = mdev_bus_register();
	if (rc < 0) {
		pr_err("Failed to register mdev bus\n");
		class_unregister(&mdev_class);
		return rc;
	}

	return rc;
}

static void __exit mdev_exit(void)
{
	mdev_bus_unregister();
	class_unregister(&mdev_class);
}

module_init(mdev_init)
module_exit(mdev_exit)

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
