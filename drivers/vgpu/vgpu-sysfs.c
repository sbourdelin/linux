/*
 * File attributes for vGPU devices
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/sysfs.h>
#include <linux/ctype.h>
#include <linux/uuid.h>
#include <linux/vfio.h>
#include <linux/vgpu.h>

#include "vgpu_private.h"

/* Prototypes */

static ssize_t vgpu_supported_types_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf);
static DEVICE_ATTR_RO(vgpu_supported_types);

static ssize_t vgpu_create_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count);
static DEVICE_ATTR_WO(vgpu_create);

static ssize_t vgpu_destroy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count);
static DEVICE_ATTR_WO(vgpu_destroy);


/* Static functions */

static bool is_uuid_sep(char sep)
{
	if (sep == '\n' || sep == '-' || sep == ':' || sep == '\0')
		return true;
	return false;
}


static int uuid_parse(const char *str, uuid_le *uuid)
{
	int i;

	if (strlen(str) < 36)
		return -1;

	for (i = 0; i < 16; i++) {
		if (!isxdigit(str[0]) || !isxdigit(str[1])) {
			printk(KERN_ERR "%s err", __FUNCTION__);
			return -EINVAL;
		}

		uuid->b[i] = (hex_to_bin(str[0]) << 4) | hex_to_bin(str[1]);
		str += 2;
		if (is_uuid_sep(*str))
			str++;
	}

	return 0;
}


/* Functions */
static ssize_t vgpu_supported_types_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	char *str;
	ssize_t n;

        str = kzalloc(sizeof(*str) * 512, GFP_KERNEL);
        if (!str)
                return -ENOMEM;

	get_vgpu_supported_types(dev, str);

	n = sprintf(buf,"%s\n", str);
	kfree(str);

	return n;
}

static ssize_t vgpu_create_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	char *str, *pstr;
	char *uuid_str, *instance_str, *vgpu_params = NULL;
	uuid_le uuid;
	uint32_t instance;
	struct pci_dev *pdev;
	int ret = 0;

	pstr = str = kstrndup(buf, count, GFP_KERNEL);

	if (!str)
		return -ENOMEM;

	if ((uuid_str = strsep(&str, ":")) == NULL) {
		printk(KERN_ERR "%s Empty UUID or string %s \n",
				 __FUNCTION__, buf);
		ret = -EINVAL;
		goto create_error;
	}

	if (!str) {
		printk(KERN_ERR "%s vgpu instance not specified %s \n",
				 __FUNCTION__, buf);
		ret = -EINVAL;
		goto create_error;
	}

	if ((instance_str = strsep(&str, ":")) == NULL) {
		printk(KERN_ERR "%s Empty instance or string %s \n",
				 __FUNCTION__, buf);
		ret = -EINVAL;
		goto create_error;
	}

	instance = (unsigned int)simple_strtoul(instance_str, NULL, 0);

	if (!str) {
		printk(KERN_ERR "%s vgpu params not specified %s \n",
				 __FUNCTION__, buf);
		ret = -EINVAL;
		goto create_error;
	}

	vgpu_params = kstrdup(str, GFP_KERNEL);

	if (!vgpu_params) {
		printk(KERN_ERR "%s vgpu params allocation failed \n",
				 __FUNCTION__);
		ret = -EINVAL;
		goto create_error;
	}

	if (uuid_parse(uuid_str, &uuid) < 0) {
		printk(KERN_ERR "%s UUID parse error  %s \n", __FUNCTION__, buf);
		ret = -EINVAL;
		goto create_error;
	}

	if (dev_is_pci(dev)) {
		pdev = to_pci_dev(dev);

		if (create_vgpu_device(pdev, uuid, instance, vgpu_params) < 0) {
			printk(KERN_ERR "%s vgpu create error \n", __FUNCTION__);
			ret = -EINVAL;
			goto create_error;
		}
		ret = count;
	}

create_error:
	if (vgpu_params)
		kfree(vgpu_params);

	if (pstr)
		kfree(pstr);
	return ret;
}

static ssize_t vgpu_destroy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	char *uuid_str, *str;
	uuid_le uuid;
	unsigned int instance;
	struct vgpu_device *vgpu_dev = NULL;

	str = kstrndup(buf, count, GFP_KERNEL);

	if (!str)
		return -ENOMEM;

	if ((uuid_str = strsep(&str, ":")) == NULL) {
		printk(KERN_ERR "%s Empty UUID or string %s \n", __FUNCTION__, buf);
		return -EINVAL;
	}

	if (str == NULL) {
		printk(KERN_ERR "%s instance not specified %s \n", __FUNCTION__, buf);
		return -EINVAL;
	}

	instance = (unsigned int)simple_strtoul(str, NULL, 0);

	if (uuid_parse(uuid_str, &uuid) < 0) {
		printk(KERN_ERR "%s UUID parse error  %s \n", __FUNCTION__, buf);
		return -EINVAL;
	}

	printk(KERN_INFO "%s UUID %pUb - %d \n", __FUNCTION__, uuid.b, instance);

	vgpu_dev = vgpu_drv_get_vgpu_device(uuid, instance);

	if (vgpu_dev)
		destroy_vgpu_device(vgpu_dev);

	return count;
}

static ssize_t
vgpu_uuid_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vgpu_device *drv = to_vgpu_device(dev);

	if (drv)
		return sprintf(buf, "%pUb \n", drv->uuid.b);

	return sprintf(buf, " \n");
}

static DEVICE_ATTR_RO(vgpu_uuid);

static ssize_t
vgpu_group_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct vgpu_device *drv = to_vgpu_device(dev);

	if (drv && drv->group)
		return sprintf(buf, "%d \n", iommu_group_id(drv->group));

	return sprintf(buf, " \n");
}

static DEVICE_ATTR_RO(vgpu_group_id);


static struct attribute *vgpu_dev_attrs[] = {
	&dev_attr_vgpu_uuid.attr,
	&dev_attr_vgpu_group_id.attr,
	NULL,
};

static const struct attribute_group vgpu_dev_group = {
	.attrs = vgpu_dev_attrs,
};

const struct attribute_group *vgpu_dev_groups[] = {
	&vgpu_dev_group,
	NULL,
};


ssize_t vgpu_start_store(struct class *class, struct class_attribute *attr,
			 const char *buf, size_t count)
{
	char *uuid_str;
	uuid_le uuid;
	struct vgpu_device *vgpu_dev = NULL;
	int ret;

	uuid_str = kstrndup(buf, count, GFP_KERNEL);

	if (!uuid_str)
		return -ENOMEM;

	if (uuid_parse(uuid_str, &uuid) < 0) {
		printk(KERN_ERR "%s UUID parse error  %s \n", __FUNCTION__, buf);
		return -EINVAL;
	}

	vgpu_dev = vgpu_drv_get_vgpu_device(uuid, 0);

	if (vgpu_dev && dev_is_vgpu(&vgpu_dev->dev)) {
		kobject_uevent(&vgpu_dev->dev.kobj, KOBJ_ONLINE);

		ret = vgpu_start_callback(vgpu_dev);
		if (ret < 0) {
			printk(KERN_ERR "%s vgpu_start callback failed  %d \n",
					 __FUNCTION__, ret);
			return ret;
		}
	}

	return count;
}

ssize_t vgpu_shutdown_store(struct class *class, struct class_attribute *attr,
			    const char *buf, size_t count)
{
	char *uuid_str;
	uuid_le uuid;
	struct vgpu_device *vgpu_dev = NULL;
	int ret;

	uuid_str = kstrndup(buf, count, GFP_KERNEL);

	if (!uuid_str)
		return -ENOMEM;

	if (uuid_parse(uuid_str, &uuid) < 0) {
		printk(KERN_ERR "%s UUID parse error  %s \n", __FUNCTION__, buf);
		return -EINVAL;
	}
	vgpu_dev = vgpu_drv_get_vgpu_device(uuid, 0);

	if (vgpu_dev && dev_is_vgpu(&vgpu_dev->dev)) {
		kobject_uevent(&vgpu_dev->dev.kobj, KOBJ_OFFLINE);

		ret = vgpu_shutdown_callback(vgpu_dev);
		if (ret < 0) {
			printk(KERN_ERR "%s vgpu_shutdown callback failed  %d \n",
					 __FUNCTION__, ret);
			return ret;
		}
	}

	return count;
}

struct class_attribute vgpu_class_attrs[] = {
	__ATTR_WO(vgpu_start),
	__ATTR_WO(vgpu_shutdown),
	__ATTR_NULL
};

int vgpu_create_pci_device_files(struct pci_dev *dev)
{
	int retval;

	retval = sysfs_create_file(&dev->dev.kobj,
				   &dev_attr_vgpu_supported_types.attr);
	if (retval) {
		printk(KERN_ERR "VGPU-VFIO: failed to create vgpu_supported_types sysfs entry\n");
		return retval;
	}

	retval = sysfs_create_file(&dev->dev.kobj, &dev_attr_vgpu_create.attr);
	if (retval) {
		printk(KERN_ERR "VGPU-VFIO: failed to create vgpu_create sysfs entry\n");
		return retval;
	}

	retval = sysfs_create_file(&dev->dev.kobj, &dev_attr_vgpu_destroy.attr);
	if (retval) {
		printk(KERN_ERR "VGPU-VFIO: failed to create vgpu_destroy sysfs entry\n");
		return retval;
	}

	return 0;
}


void vgpu_remove_pci_device_files(struct pci_dev *dev)
{
	sysfs_remove_file(&dev->dev.kobj, &dev_attr_vgpu_supported_types.attr);
	sysfs_remove_file(&dev->dev.kobj, &dev_attr_vgpu_create.attr);
	sysfs_remove_file(&dev->dev.kobj, &dev_attr_vgpu_destroy.attr);
}
