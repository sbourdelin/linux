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

#define DRIVER_VERSION		"0.1"
#define DRIVER_AUTHOR		"NVIDIA Corporation"
#define DRIVER_DESC		"Mediated device Core Driver"

#define MDEV_CLASS_NAME		"mdev"

static LIST_HEAD(parent_list);
static DEFINE_MUTEX(parent_list_lock);

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

/* Should be called holding parent->mdev_list_lock */
static struct mdev_device *find_mdev_device(struct parent_device *parent,
					    uuid_le uuid, int instance)
{
	struct mdev_device *mdev;

	list_for_each_entry(mdev, &parent->mdev_list, next) {
		if ((uuid_le_cmp(mdev->uuid, uuid) == 0) &&
		    (mdev->instance == instance))
			return mdev;
	}
	return NULL;
}

/* Should be called holding parent_list_lock */
static struct parent_device *find_parent_device(struct device *dev)
{
	struct parent_device *parent;

	list_for_each_entry(parent, &parent_list, next) {
		if (parent->dev == dev)
			return parent;
	}
	return NULL;
}

static void mdev_release_parent(struct kref *kref)
{
	struct parent_device *parent = container_of(kref, struct parent_device,
						    ref);
	kfree(parent);
}

static
inline struct parent_device *mdev_get_parent(struct parent_device *parent)
{
	if (parent)
		kref_get(&parent->ref);

	return parent;
}

static inline void mdev_put_parent(struct parent_device *parent)
{
	if (parent)
		kref_put(&parent->ref, mdev_release_parent);
}

static struct parent_device *mdev_get_parent_by_dev(struct device *dev)
{
	struct parent_device *parent = NULL, *p;

	mutex_lock(&parent_list_lock);
	list_for_each_entry(p, &parent_list, next) {
		if (p->dev == dev) {
			parent = mdev_get_parent(p);
			break;
		}
	}
	mutex_unlock(&parent_list_lock);
	return parent;
}

static int mdev_device_create_ops(struct mdev_device *mdev, char *mdev_params)
{
	struct parent_device *parent = mdev->parent;
	int ret;

	ret = parent->ops->create(mdev, mdev_params);
	if (ret)
		return ret;

	ret = mdev_add_attribute_group(&mdev->dev,
					parent->ops->mdev_attr_groups);
	if (ret)
		parent->ops->destroy(mdev);

	return ret;
}

static int mdev_device_destroy_ops(struct mdev_device *mdev, bool force)
{
	struct parent_device *parent = mdev->parent;
	int ret = 0;

	/*
	 * If vendor driver doesn't return success that means vendor
	 * driver doesn't support hot-unplug
	 */
	ret = parent->ops->destroy(mdev);
	if (ret && !force)
		return -EBUSY;

	mdev_remove_attribute_group(&mdev->dev,
				    parent->ops->mdev_attr_groups);

	return ret;
}

static void mdev_release_device(struct kref *kref)
{
	struct mdev_device *mdev = container_of(kref, struct mdev_device, ref);
	struct parent_device *parent = mdev->parent;

	list_del(&mdev->next);
	mutex_unlock(&parent->mdev_list_lock);

	device_unregister(&mdev->dev);
	wake_up(&parent->release_done);
	mdev_put_parent(parent);
}

struct mdev_device *mdev_get_device(struct mdev_device *mdev)
{
	kref_get(&mdev->ref);
	return mdev;
}
EXPORT_SYMBOL(mdev_get_device);

void mdev_put_device(struct mdev_device *mdev)
{
	struct parent_device *parent = mdev->parent;

	kref_put_mutex(&mdev->ref, mdev_release_device,
		       &parent->mdev_list_lock);
}
EXPORT_SYMBOL(mdev_put_device);

/*
 * Find first mediated device from given uuid and increment refcount of
 * mediated device. Caller should call mdev_put_device() when the use of
 * mdev_device is done.
 */
static struct mdev_device *mdev_get_first_device_by_uuid(uuid_le uuid)
{
	struct mdev_device *mdev = NULL, *p;
	struct parent_device *parent;

	mutex_lock(&parent_list_lock);
	list_for_each_entry(parent, &parent_list, next) {
		mutex_lock(&parent->mdev_list_lock);
		list_for_each_entry(p, &parent->mdev_list, next) {
			if (uuid_le_cmp(p->uuid, uuid) == 0) {
				mdev = mdev_get_device(p);
				break;
			}
		}
		mutex_unlock(&parent->mdev_list_lock);

		if (mdev)
			break;
	}
	mutex_unlock(&parent_list_lock);
	return mdev;
}

/*
 * Find mediated device from given iommu_group and increment refcount of
 * mediated device. Caller should call mdev_put_device() when the use of
 * mdev_device is done.
 */
struct mdev_device *mdev_get_device_by_group(struct iommu_group *group)
{
	struct mdev_device *mdev = NULL, *p;
	struct parent_device *parent;

	mutex_lock(&parent_list_lock);
	list_for_each_entry(parent, &parent_list, next) {
		mutex_lock(&parent->mdev_list_lock);
		list_for_each_entry(p, &parent->mdev_list, next) {
			if (!p->group)
				continue;

			if (iommu_group_id(p->group) == iommu_group_id(group)) {
				mdev = mdev_get_device(p);
				break;
			}
		}
		mutex_unlock(&parent->mdev_list_lock);

		if (mdev)
			break;
	}
	mutex_unlock(&parent_list_lock);
	return mdev;
}
EXPORT_SYMBOL(mdev_get_device_by_group);

/*
 * mdev_register_device : Register a device
 * @dev: device structure representing parent device.
 * @ops: Parent device operation structure to be registered.
 *
 * Add device to list of registered parent devices.
 * Returns a negative value on error, otherwise 0.
 */
int mdev_register_device(struct device *dev, const struct parent_ops *ops)
{
	int ret = 0;
	struct parent_device *parent;

	if (!dev || !ops)
		return -EINVAL;

	/* check for mandatory ops */
	if (!ops->create || !ops->destroy)
		return -EINVAL;

	mutex_lock(&parent_list_lock);

	/* Check for duplicate */
	parent = find_parent_device(dev);
	if (parent) {
		ret = -EEXIST;
		goto add_dev_err;
	}

	parent = kzalloc(sizeof(*parent), GFP_KERNEL);
	if (!parent) {
		ret = -ENOMEM;
		goto add_dev_err;
	}

	kref_init(&parent->ref);
	list_add(&parent->next, &parent_list);

	parent->dev = dev;
	parent->ops = ops;
	mutex_init(&parent->mdev_list_lock);
	INIT_LIST_HEAD(&parent->mdev_list);
	init_waitqueue_head(&parent->release_done);
	mutex_unlock(&parent_list_lock);

	ret = mdev_create_sysfs_files(dev);
	if (ret)
		goto add_sysfs_error;

	ret = mdev_add_attribute_group(dev, ops->dev_attr_groups);
	if (ret)
		goto add_group_error;

	dev_info(dev, "MDEV: Registered\n");
	return 0;

add_group_error:
	mdev_remove_sysfs_files(dev);
add_sysfs_error:
	mutex_lock(&parent_list_lock);
	list_del(&parent->next);
	mutex_unlock(&parent_list_lock);
	mdev_put_parent(parent);
	return ret;

add_dev_err:
	mutex_unlock(&parent_list_lock);
	return ret;
}
EXPORT_SYMBOL(mdev_register_device);

/*
 * mdev_unregister_device : Unregister a parent device
 * @dev: device structure representing parent device.
 *
 * Remove device from list of registered parent devices. Give a chance to free
 * existing mediated devices for given device.
 */

void mdev_unregister_device(struct device *dev)
{
	struct parent_device *parent;
	struct mdev_device *mdev, *n;
	int ret;

	mutex_lock(&parent_list_lock);
	parent = find_parent_device(dev);

	if (!parent) {
		mutex_unlock(&parent_list_lock);
		return;
	}
	dev_info(dev, "MDEV: Unregistering\n");

	/*
	 * Remove parent from the list and remove create and destroy sysfs
	 * files so that no new mediated device could be created for this parent
	 */
	list_del(&parent->next);
	mdev_remove_sysfs_files(dev);
	mutex_unlock(&parent_list_lock);

	mdev_remove_attribute_group(dev,
				    parent->ops->dev_attr_groups);

	mutex_lock(&parent->mdev_list_lock);
	list_for_each_entry_safe(mdev, n, &parent->mdev_list, next) {
		mdev_device_destroy_ops(mdev, true);
		mutex_unlock(&parent->mdev_list_lock);
		mdev_put_device(mdev);
		mutex_lock(&parent->mdev_list_lock);
	}
	mutex_unlock(&parent->mdev_list_lock);

	do {
		ret = wait_event_interruptible_timeout(parent->release_done,
				list_empty(&parent->mdev_list), HZ * 10);
		if (ret == -ERESTARTSYS) {
			dev_warn(dev, "Mediated devices are in use, task"
				      " \"%s\" (%d) "
				      "blocked until all are released",
				      current->comm, task_pid_nr(current));
		}
	} while (ret <= 0);

	mdev_put_parent(parent);
}
EXPORT_SYMBOL(mdev_unregister_device);

/*
 * Functions required for mdev_sysfs
 */
static void mdev_device_release(struct device *dev)
{
	struct mdev_device *mdev = to_mdev_device(dev);

	dev_dbg(&mdev->dev, "MDEV: destroying\n");
	kfree(mdev);
}

int mdev_device_create(struct device *dev, uuid_le uuid, uint32_t instance,
		       char *mdev_params)
{
	int ret;
	struct mdev_device *mdev;
	struct parent_device *parent;

	parent = mdev_get_parent_by_dev(dev);
	if (!parent)
		return -EINVAL;

	mutex_lock(&parent->mdev_list_lock);
	/* Check for duplicate */
	mdev = find_mdev_device(parent, uuid, instance);
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
	mdev->instance = instance;
	mdev->parent = parent;
	kref_init(&mdev->ref);

	mdev->dev.parent  = dev;
	mdev->dev.bus     = &mdev_bus_type;
	mdev->dev.release = mdev_device_release;
	dev_set_name(&mdev->dev, "%pUl-%d", uuid.b, instance);

	ret = device_register(&mdev->dev);
	if (ret) {
		put_device(&mdev->dev);
		goto create_err;
	}

	ret = mdev_device_create_ops(mdev, mdev_params);
	if (ret)
		goto create_failed;

	list_add(&mdev->next, &parent->mdev_list);
	mutex_unlock(&parent->mdev_list_lock);

	dev_dbg(&mdev->dev, "MDEV: created\n");

	return ret;

create_failed:
	device_unregister(&mdev->dev);

create_err:
	mutex_unlock(&parent->mdev_list_lock);
	mdev_put_parent(parent);
	return ret;
}

int mdev_device_destroy(struct device *dev, uuid_le uuid, uint32_t instance)
{
	struct mdev_device *mdev;
	struct parent_device *parent;
	int ret;

	parent = mdev_get_parent_by_dev(dev);
	if (!parent)
		return -EINVAL;

	mutex_lock(&parent->mdev_list_lock);
	mdev = find_mdev_device(parent, uuid, instance);
	if (!mdev) {
		ret = -EINVAL;
		goto destroy_err;
	}

	ret = mdev_device_destroy_ops(mdev, false);
	if (ret)
		goto destroy_err;

	mutex_unlock(&parent->mdev_list_lock);
	mdev_put_device(mdev);

	mdev_put_parent(parent);
	return ret;

destroy_err:
	mutex_unlock(&parent->mdev_list_lock);
	mdev_put_parent(parent);
	return ret;
}

int mdev_device_invalidate_mapping(struct mdev_device *mdev,
				   unsigned long addr, unsigned long size)
{
	int ret = -EINVAL;
	struct mdev_phys_mapping *phys_mappings;
	struct addr_desc *addr_desc;

	if (!mdev || !mdev->phys_mappings.mapping)
		return ret;

	phys_mappings = &mdev->phys_mappings;

	mutex_lock(&phys_mappings->addr_desc_list_lock);

	list_for_each_entry(addr_desc, &phys_mappings->addr_desc_list, next) {

		if ((addr > addr_desc->start) &&
		    (addr + size < addr_desc->start + addr_desc->size)) {
			unmap_mapping_range(phys_mappings->mapping,
					    addr, size, 0);
			ret = 0;
			goto unlock_exit;
		}
	}

unlock_exit:
	mutex_unlock(&phys_mappings->addr_desc_list_lock);
	return ret;
}
EXPORT_SYMBOL(mdev_device_invalidate_mapping);

/* Sanity check for the physical mapping list for mediated device */

int mdev_add_phys_mapping(struct mdev_device *mdev,
			  struct address_space *mapping,
			  unsigned long addr, unsigned long size)
{
	struct mdev_phys_mapping *phys_mappings;
	struct addr_desc *addr_desc, *new_addr_desc;
	int ret = 0;

	if (!mdev)
		return -EINVAL;

	phys_mappings = &mdev->phys_mappings;
	if (phys_mappings->mapping && (mapping != phys_mappings->mapping))
		return -EINVAL;

	if (!phys_mappings->mapping) {
		phys_mappings->mapping = mapping;
		mutex_init(&phys_mappings->addr_desc_list_lock);
		INIT_LIST_HEAD(&phys_mappings->addr_desc_list);
	}

	mutex_lock(&phys_mappings->addr_desc_list_lock);

	list_for_each_entry(addr_desc, &phys_mappings->addr_desc_list, next) {
		if ((addr + size < addr_desc->start) ||
		    (addr_desc->start + addr_desc->size) < addr)
			continue;
		else {
			/* should be no overlap */
			ret = -EINVAL;
			goto mapping_exit;
		}
	}

	/* add the new entry to the list */
	new_addr_desc = kzalloc(sizeof(*new_addr_desc), GFP_KERNEL);

	if (!new_addr_desc) {
		ret = -ENOMEM;
		goto mapping_exit;
	}

	new_addr_desc->start = addr;
	new_addr_desc->size = size;
	list_add(&new_addr_desc->next, &phys_mappings->addr_desc_list);

mapping_exit:
	mutex_unlock(&phys_mappings->addr_desc_list_lock);
	return ret;
}
EXPORT_SYMBOL(mdev_add_phys_mapping);

void mdev_del_phys_mapping(struct mdev_device *mdev, unsigned long addr)
{
	struct mdev_phys_mapping *phys_mappings;
	struct addr_desc *addr_desc;

	if (!mdev)
		return;

	phys_mappings = &mdev->phys_mappings;

	mutex_lock(&phys_mappings->addr_desc_list_lock);
	list_for_each_entry(addr_desc, &phys_mappings->addr_desc_list, next) {
		if (addr_desc->start == addr) {
			list_del(&addr_desc->next);
			kfree(addr_desc);
			break;
		}
	}
	mutex_unlock(&phys_mappings->addr_desc_list_lock);
}
EXPORT_SYMBOL(mdev_del_phys_mapping);

void mdev_device_supported_config(struct device *dev, char *str)
{
	struct parent_device *parent;

	parent = mdev_get_parent_by_dev(dev);

	if (parent) {
		if (parent->ops->supported_config)
			parent->ops->supported_config(parent->dev, str);
		mdev_put_parent(parent);
	}
}

int mdev_device_start(uuid_le uuid)
{
	int ret = 0;
	struct mdev_device *mdev;
	struct parent_device *parent;

	mdev = mdev_get_first_device_by_uuid(uuid);
	if (!mdev)
		return -EINVAL;

	parent = mdev->parent;

	if (parent->ops->start)
		ret = parent->ops->start(mdev->uuid);

	if (ret)
		pr_err("mdev_start failed  %d\n", ret);
	else
		kobject_uevent(&mdev->dev.kobj, KOBJ_ONLINE);

	mdev_put_device(mdev);

	return ret;
}

int mdev_device_stop(uuid_le uuid)
{
	int ret = 0;
	struct mdev_device *mdev;
	struct parent_device *parent;

	mdev = mdev_get_first_device_by_uuid(uuid);
	if (!mdev)
		return -EINVAL;

	parent = mdev->parent;

	if (parent->ops->stop)
		ret = parent->ops->stop(mdev->uuid);

	if (ret)
		pr_err("mdev stop failed %d\n", ret);
	else
		kobject_uevent(&mdev->dev.kobj, KOBJ_OFFLINE);

	mdev_put_device(mdev);
	return ret;
}

static struct class mdev_class = {
	.name		= MDEV_CLASS_NAME,
	.owner		= THIS_MODULE,
	.class_attrs	= mdev_class_attrs,
};

static int __init mdev_init(void)
{
	int ret;

	ret = class_register(&mdev_class);
	if (ret) {
		pr_err("Failed to register mdev class\n");
		return ret;
	}

	ret = mdev_bus_register();
	if (ret) {
		pr_err("Failed to register mdev bus\n");
		class_unregister(&mdev_class);
		return ret;
	}

	return ret;
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
