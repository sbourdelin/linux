/*
 * VGPU Core Driver
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
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uuid.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/sysfs.h>
#include <linux/ctype.h>
#include <linux/vgpu.h>

#include "vgpu_private.h"

#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"NVIDIA Corporation"
#define DRIVER_DESC	"VGPU Core Driver"

/*
 * #defines
 */

#define VGPU_CLASS_NAME		"vgpu"

/*
 * Global Structures
 */

static struct vgpu {
	struct list_head    vgpu_devices_list;
	struct mutex        vgpu_devices_lock;
	struct list_head    gpu_devices_list;
	struct mutex        gpu_devices_lock;
} vgpu;

static struct class vgpu_class;

/*
 * Functions
 */

struct vgpu_device *get_vgpu_device_from_group(struct iommu_group *group)
{
	struct vgpu_device *vdev = NULL;

	mutex_lock(&vgpu.vgpu_devices_lock);
	list_for_each_entry(vdev, &vgpu.vgpu_devices_list, list) {
		if (vdev->group) {
			if (iommu_group_id(vdev->group) == iommu_group_id(group)) {
				mutex_unlock(&vgpu.vgpu_devices_lock);
				return vdev;
			}
		}
	}
	mutex_unlock(&vgpu.vgpu_devices_lock);
	return NULL;
}

EXPORT_SYMBOL_GPL(get_vgpu_device_from_group);

static int vgpu_add_attribute_group(struct device *dev,
			            const struct attribute_group **groups)
{
        return sysfs_create_groups(&dev->kobj, groups);
}

static void vgpu_remove_attribute_group(struct device *dev,
			                const struct attribute_group **groups)
{
        sysfs_remove_groups(&dev->kobj, groups);
}

int vgpu_register_device(struct pci_dev *dev, const struct gpu_device_ops *ops)
{
	int ret = 0;
	struct gpu_device *gpu_dev, *tmp;

	if (!dev)
		return -EINVAL;

        gpu_dev = kzalloc(sizeof(*gpu_dev), GFP_KERNEL);
        if (!gpu_dev)
                return -ENOMEM;

	gpu_dev->dev = dev;
        gpu_dev->ops = ops;

        mutex_lock(&vgpu.gpu_devices_lock);

        /* Check for duplicates */
        list_for_each_entry(tmp, &vgpu.gpu_devices_list, gpu_next) {
                if (tmp->dev == dev) {
			ret = -EINVAL;
			goto add_error;
                }
        }

	ret = vgpu_create_pci_device_files(dev);
	if (ret)
		goto add_error;

	ret = vgpu_add_attribute_group(&dev->dev, ops->dev_attr_groups);
	if (ret)
		goto add_group_error;

        list_add(&gpu_dev->gpu_next, &vgpu.gpu_devices_list);

	printk(KERN_INFO "VGPU: Registered dev 0x%x 0x%x, class 0x%x\n",
			 dev->vendor, dev->device, dev->class);
        mutex_unlock(&vgpu.gpu_devices_lock);

        return 0;

add_group_error:
	vgpu_remove_pci_device_files(dev);
add_error:
	mutex_unlock(&vgpu.gpu_devices_lock);
	kfree(gpu_dev);
	return ret;

}
EXPORT_SYMBOL(vgpu_register_device);

void vgpu_unregister_device(struct pci_dev *dev)
{
        struct gpu_device *gpu_dev;

        mutex_lock(&vgpu.gpu_devices_lock);
        list_for_each_entry(gpu_dev, &vgpu.gpu_devices_list, gpu_next) {
		struct vgpu_device *vdev = NULL;

                if (gpu_dev->dev != dev)
			continue;

		printk(KERN_INFO "VGPU: Unregistered dev 0x%x 0x%x, class 0x%x\n",
				dev->vendor, dev->device, dev->class);

		list_for_each_entry(vdev, &vgpu.vgpu_devices_list, list) {
			if (vdev->gpu_dev != gpu_dev)
				continue;
			destroy_vgpu_device(vdev);
		}
		vgpu_remove_attribute_group(&dev->dev, gpu_dev->ops->dev_attr_groups);
		vgpu_remove_pci_device_files(dev);
		list_del(&gpu_dev->gpu_next);
		mutex_unlock(&vgpu.gpu_devices_lock);
		kfree(gpu_dev);
		return;
        }
        mutex_unlock(&vgpu.gpu_devices_lock);
}
EXPORT_SYMBOL(vgpu_unregister_device);

/*
 * Helper Functions
 */

static struct vgpu_device *vgpu_device_alloc(uuid_le uuid, int instance, char *name)
{
	struct vgpu_device *vgpu_dev = NULL;

	vgpu_dev = kzalloc(sizeof(*vgpu_dev), GFP_KERNEL);
	if (!vgpu_dev)
		return ERR_PTR(-ENOMEM);

	kref_init(&vgpu_dev->kref);
	memcpy(&vgpu_dev->uuid, &uuid, sizeof(uuid_le));
	vgpu_dev->vgpu_instance = instance;
	strcpy(vgpu_dev->dev_name, name);

	mutex_lock(&vgpu.vgpu_devices_lock);
	list_add(&vgpu_dev->list, &vgpu.vgpu_devices_list);
	mutex_unlock(&vgpu.vgpu_devices_lock);

	return vgpu_dev;
}

static void vgpu_device_free(struct vgpu_device *vgpu_dev)
{
	if (vgpu_dev) {
		mutex_lock(&vgpu.vgpu_devices_lock);
		list_del(&vgpu_dev->list);
		mutex_unlock(&vgpu.vgpu_devices_lock);
		kfree(vgpu_dev);
	}
	return;
}

struct vgpu_device *vgpu_drv_get_vgpu_device(uuid_le uuid, int instance)
{
	struct vgpu_device *vdev = NULL;

	mutex_lock(&vgpu.vgpu_devices_lock);
	list_for_each_entry(vdev, &vgpu.vgpu_devices_list, list) {
		if ((uuid_le_cmp(vdev->uuid, uuid) == 0) &&
		    (vdev->vgpu_instance == instance)) {
			mutex_unlock(&vgpu.vgpu_devices_lock);
			return vdev;
		}
	}
	mutex_unlock(&vgpu.vgpu_devices_lock);
	return NULL;
}

static void vgpu_device_release(struct device *dev)
{
	struct vgpu_device *vgpu_dev = to_vgpu_device(dev);
	vgpu_device_free(vgpu_dev);
}

int create_vgpu_device(struct pci_dev *pdev, uuid_le uuid, uint32_t instance, char *vgpu_params)
{
	char name[64];
	int numChar = 0;
	int retval = 0;
	struct vgpu_device *vgpu_dev = NULL;
	struct gpu_device *gpu_dev;

	printk(KERN_INFO "VGPU: %s: device ", __FUNCTION__);

	numChar = sprintf(name, "%pUb-%d", uuid.b, instance);
	name[numChar] = '\0';

	vgpu_dev = vgpu_device_alloc(uuid, instance, name);
	if (IS_ERR(vgpu_dev)) {
		return PTR_ERR(vgpu_dev);
	}

	vgpu_dev->dev.parent  = &pdev->dev;
	vgpu_dev->dev.bus     = &vgpu_bus_type;
	vgpu_dev->dev.release = vgpu_device_release;
	dev_set_name(&vgpu_dev->dev, "%s", name);

	retval = device_register(&vgpu_dev->dev);
	if (retval)
		goto create_failed1;

	printk(KERN_INFO "UUID %pUb \n", vgpu_dev->uuid.b);

	mutex_lock(&vgpu.gpu_devices_lock);
	list_for_each_entry(gpu_dev, &vgpu.gpu_devices_list, gpu_next) {
		if (gpu_dev->dev != pdev)
			continue;

		vgpu_dev->gpu_dev = gpu_dev;
		if (gpu_dev->ops->vgpu_create) {
			retval = gpu_dev->ops->vgpu_create(pdev, vgpu_dev->uuid,
							   instance, vgpu_params);
			if (retval) {
				mutex_unlock(&vgpu.gpu_devices_lock);
				goto create_failed2;
			}
		}
		break;
	}
	if (!vgpu_dev->gpu_dev) {
		retval = -EINVAL;
		mutex_unlock(&vgpu.gpu_devices_lock);
		goto create_failed2;
	}

	mutex_unlock(&vgpu.gpu_devices_lock);

	retval = vgpu_add_attribute_group(&vgpu_dev->dev, gpu_dev->ops->vgpu_attr_groups);
	if (retval)
		goto create_attr_error;

	return retval;

create_attr_error:
	if (gpu_dev->ops->vgpu_destroy) {
		int ret = 0;
		ret = gpu_dev->ops->vgpu_destroy(gpu_dev->dev,
						 vgpu_dev->uuid,
						 vgpu_dev->vgpu_instance);
	}

create_failed2:
	device_unregister(&vgpu_dev->dev);

create_failed1:
	vgpu_device_free(vgpu_dev);

	return retval;
}

void destroy_vgpu_device(struct vgpu_device *vgpu_dev)
{
	struct gpu_device *gpu_dev = vgpu_dev->gpu_dev;

	printk(KERN_INFO "VGPU: destroying device %s ", vgpu_dev->dev_name);
	if (gpu_dev->ops->vgpu_destroy) {
		int retval = 0;
		retval = gpu_dev->ops->vgpu_destroy(gpu_dev->dev,
						    vgpu_dev->uuid,
						    vgpu_dev->vgpu_instance);
	/* if vendor driver doesn't return success that means vendor driver doesn't
	 * support hot-unplug */
		if (retval)
			return;
	}

	vgpu_remove_attribute_group(&vgpu_dev->dev, gpu_dev->ops->vgpu_attr_groups);
	device_unregister(&vgpu_dev->dev);
}

void get_vgpu_supported_types(struct device *dev, char *str)
{
	struct gpu_device *gpu_dev;

	mutex_lock(&vgpu.gpu_devices_lock);
	list_for_each_entry(gpu_dev, &vgpu.gpu_devices_list, gpu_next) {
		if (&gpu_dev->dev->dev == dev) {
			if (gpu_dev->ops->vgpu_supported_config)
				gpu_dev->ops->vgpu_supported_config(gpu_dev->dev, str);
			break;
		}
	}
	mutex_unlock(&vgpu.gpu_devices_lock);
}

int vgpu_start_callback(struct vgpu_device *vgpu_dev)
{
	int ret = 0;
	struct gpu_device *gpu_dev = vgpu_dev->gpu_dev;

	mutex_lock(&vgpu.gpu_devices_lock);
	if (gpu_dev->ops->vgpu_start)
		ret = gpu_dev->ops->vgpu_start(vgpu_dev->uuid);
	mutex_unlock(&vgpu.gpu_devices_lock);
	return ret;
}

int vgpu_shutdown_callback(struct vgpu_device *vgpu_dev)
{
	int ret = 0;
	struct gpu_device *gpu_dev = vgpu_dev->gpu_dev;

	mutex_lock(&vgpu.gpu_devices_lock);
	if (gpu_dev->ops->vgpu_shutdown)
		ret = gpu_dev->ops->vgpu_shutdown(vgpu_dev->uuid);
	mutex_unlock(&vgpu.gpu_devices_lock);
	return ret;
}

char *vgpu_devnode(struct device *dev, umode_t *mode)
{
	return kasprintf(GFP_KERNEL, "vgpu/%s", dev_name(dev));
}

static void release_vgpubus_dev(struct device *dev)
{
	struct vgpu_device *vgpu_dev = to_vgpu_device(dev);
	destroy_vgpu_device(vgpu_dev);
}

static struct class vgpu_class = {
	.name		= VGPU_CLASS_NAME,
	.owner		= THIS_MODULE,
	.class_attrs	= vgpu_class_attrs,
	.dev_groups	= vgpu_dev_groups,
	.devnode	= vgpu_devnode,
	.dev_release    = release_vgpubus_dev,
};

static int __init vgpu_init(void)
{
	int rc = 0;

	memset(&vgpu, 0 , sizeof(vgpu));

	mutex_init(&vgpu.vgpu_devices_lock);
	INIT_LIST_HEAD(&vgpu.vgpu_devices_list);
	mutex_init(&vgpu.gpu_devices_lock);
	INIT_LIST_HEAD(&vgpu.gpu_devices_list);

	rc = class_register(&vgpu_class);
	if (rc < 0) {
		printk(KERN_ERR "Error: failed to register vgpu class\n");
		goto failed1;
	}

	rc = vgpu_bus_register();
	if (rc < 0) {
		printk(KERN_ERR "Error: failed to register vgpu bus\n");
		class_unregister(&vgpu_class);
	}

    request_module_nowait("vgpu_vfio");

failed1:
	return rc;
}

static void __exit vgpu_exit(void)
{
	vgpu_bus_unregister();
	class_unregister(&vgpu_class);
}

module_init(vgpu_init)
module_exit(vgpu_exit)

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
