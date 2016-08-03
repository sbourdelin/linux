/*
 * File attributes for Mediated devices
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/sysfs.h>
#include <linux/ctype.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/mdev.h>

#include "mdev_private.h"

/* Prototypes */
static ssize_t mdev_supported_types_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf);
static DEVICE_ATTR_RO(mdev_supported_types);

static ssize_t mdev_create_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count);
static DEVICE_ATTR_WO(mdev_create);

static ssize_t mdev_destroy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count);
static DEVICE_ATTR_WO(mdev_destroy);

/* Static functions */


#define SUPPORTED_TYPE_BUFFER_LENGTH	4096

/* mdev sysfs Functions */
static ssize_t mdev_supported_types_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	char *str, *ptr;
	ssize_t n;

	str = kzalloc(sizeof(*str) * SUPPORTED_TYPE_BUFFER_LENGTH, GFP_KERNEL);
	if (!str)
		return -ENOMEM;

	ptr = str;
	mdev_device_supported_config(dev, str);

	n = sprintf(buf, "%s\n", str);
	kfree(ptr);

	return n;
}

static ssize_t mdev_create_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	char *str, *pstr;
	char *uuid_str, *instance_str, *mdev_params = NULL, *params = NULL;
	uuid_le uuid;
	uint32_t instance;
	int ret;

	pstr = str = kstrndup(buf, count, GFP_KERNEL);

	if (!str)
		return -ENOMEM;

	uuid_str = strsep(&str, ":");
	if (!uuid_str) {
		pr_err("mdev_create: Empty UUID string %s\n", buf);
		ret = -EINVAL;
		goto create_error;
	}

	if (!str) {
		pr_err("mdev_create: mdev instance not present %s\n", buf);
		ret = -EINVAL;
		goto create_error;
	}

	instance_str = strsep(&str, ":");
	if (!instance_str) {
		pr_err("mdev_create: Empty instance string %s\n", buf);
		ret = -EINVAL;
		goto create_error;
	}

	ret = kstrtouint(instance_str, 0, &instance);
	if (ret) {
		pr_err("mdev_create: mdev instance parsing error %s\n", buf);
		goto create_error;
	}

	if (str)
		params = mdev_params = kstrdup(str, GFP_KERNEL);

	ret = uuid_le_to_bin(uuid_str, &uuid);
	if (ret) {
		pr_err("mdev_create: UUID parse error %s\n", buf);
		goto create_error;
	}

	ret = mdev_device_create(dev, uuid, instance, mdev_params);
	if (ret)
		pr_err("mdev_create: Failed to create mdev device\n");
	else
		ret = count;

create_error:
	kfree(params);
	kfree(pstr);
	return ret;
}

static ssize_t mdev_destroy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	char *uuid_str, *str, *pstr;
	uuid_le uuid;
	unsigned int instance;
	int ret;

	str = pstr = kstrndup(buf, count, GFP_KERNEL);

	if (!str)
		return -ENOMEM;

	uuid_str = strsep(&str, ":");
	if (!uuid_str) {
		pr_err("mdev_destroy: Empty UUID string %s\n", buf);
		ret = -EINVAL;
		goto destroy_error;
	}

	if (str == NULL) {
		pr_err("mdev_destroy: instance not specified %s\n", buf);
		ret = -EINVAL;
		goto destroy_error;
	}

	ret = kstrtouint(str, 0, &instance);
	if (ret) {
		pr_err("mdev_destroy: instance parsing error %s\n", buf);
		goto destroy_error;
	}

	ret = uuid_le_to_bin(uuid_str, &uuid);
	if (ret) {
		pr_err("mdev_destroy: UUID parse error  %s\n", buf);
		goto destroy_error;
	}

	ret = mdev_device_destroy(dev, uuid, instance);
	if (ret == 0)
		ret = count;

destroy_error:
	kfree(pstr);
	return ret;
}

ssize_t mdev_start_store(struct class *class, struct class_attribute *attr,
			 const char *buf, size_t count)
{
	char *uuid_str, *ptr;
	uuid_le uuid;
	int ret;

	ptr = uuid_str = kstrndup(buf, count, GFP_KERNEL);

	if (!uuid_str)
		return -ENOMEM;

	ret = uuid_le_to_bin(uuid_str, &uuid);
	if (ret) {
		pr_err("mdev_start: UUID parse error  %s\n", buf);
		goto start_error;
	}

	ret = mdev_device_start(uuid);
	if (ret == 0)
		ret = count;

start_error:
	kfree(ptr);
	return ret;
}

ssize_t mdev_stop_store(struct class *class, struct class_attribute *attr,
			    const char *buf, size_t count)
{
	char *uuid_str, *ptr;
	uuid_le uuid;
	int ret;

	ptr = uuid_str = kstrndup(buf, count, GFP_KERNEL);

	if (!uuid_str)
		return -ENOMEM;

	ret = uuid_le_to_bin(uuid_str, &uuid);
	if (ret) {
		pr_err("mdev_stop: UUID parse error %s\n", buf);
		goto stop_error;
	}

	ret = mdev_device_stop(uuid);
	if (ret == 0)
		ret = count;

stop_error:
	kfree(ptr);
	return ret;

}

struct class_attribute mdev_class_attrs[] = {
	__ATTR_WO(mdev_start),
	__ATTR_WO(mdev_stop),
	__ATTR_NULL
};

int mdev_create_sysfs_files(struct device *dev)
{
	int ret;

	ret = sysfs_create_file(&dev->kobj,
				&dev_attr_mdev_supported_types.attr);
	if (ret) {
		pr_err("Failed to create mdev_supported_types sysfs entry\n");
		return ret;
	}

	ret = sysfs_create_file(&dev->kobj, &dev_attr_mdev_create.attr);
	if (ret) {
		pr_err("Failed to create mdev_create sysfs entry\n");
		goto create_sysfs_failed;
	}

	ret = sysfs_create_file(&dev->kobj, &dev_attr_mdev_destroy.attr);
	if (ret) {
		pr_err("Failed to create mdev_destroy sysfs entry\n");
		sysfs_remove_file(&dev->kobj, &dev_attr_mdev_create.attr);
	} else
		return ret;

create_sysfs_failed:
	sysfs_remove_file(&dev->kobj, &dev_attr_mdev_supported_types.attr);
	return ret;
}

void mdev_remove_sysfs_files(struct device *dev)
{
	sysfs_remove_file(&dev->kobj, &dev_attr_mdev_supported_types.attr);
	sysfs_remove_file(&dev->kobj, &dev_attr_mdev_create.attr);
	sysfs_remove_file(&dev->kobj, &dev_attr_mdev_destroy.attr);
}
