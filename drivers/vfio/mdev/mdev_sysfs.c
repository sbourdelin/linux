/*
 * File attributes for Mediated devices
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * Copyright (c) 2016 Intel Corporation.
 * Author:
 *	Jike Song <jike.song@intel.com>
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

static const struct attribute *mdev_host_attrs[] = {
	&dev_attr_mdev_supported_types.attr,
	&dev_attr_mdev_create.attr,
	&dev_attr_mdev_destroy.attr,
	NULL,
};


#define SUPPORTED_TYPE_BUFFER_LENGTH	4096

/* mdev host sysfs functions */
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
	char *str;
	char *uuid_str, *params = NULL;
	uuid_le uuid;
	int ret;

	str = kstrndup(buf, count, GFP_KERNEL);
	if (!str)
		return -ENOMEM;

	uuid_str = strsep(&str, ":");
	if (!uuid_str) {
		pr_err("mdev_create: empty UUID string %s\n", buf);
		ret = -EINVAL;
		goto create_error;
	}

	if (str)
		params = kstrdup(str, GFP_KERNEL);

	ret = uuid_le_to_bin(uuid_str, &uuid);
	if (ret) {
		pr_err("mdev_create: UUID parse error %s\n", buf);
		goto create_error;
	}

	ret = mdev_device_create(dev, uuid, params);
	if (ret)
		pr_err("mdev_create: Failed to create mdev device\n");
	else
		ret = count;

create_error:
	kfree(params);
	kfree(str);
	return ret;
}

static ssize_t mdev_destroy_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	char *str;
	uuid_le uuid;
	int ret;

	str = kstrndup(buf, count, GFP_KERNEL);
	if (!str)
		return -ENOMEM;

	ret = uuid_le_to_bin(str, &uuid);
	if (ret) {
		pr_err("mdev_destroy: UUID parse error  %s\n", buf);
		goto destroy_error;
	}

	ret = mdev_device_destroy(dev, uuid);
	if (ret == 0)
		ret = count;

destroy_error:
	kfree(str);
	return ret;
}

int mdev_create_sysfs_files(struct device *dev)
{
	int ret;

	ret = sysfs_create_files(&dev->kobj, mdev_host_attrs);
	if (ret)
		pr_err("sysfs_create_files failed: %d\n", ret);

	return ret;
}

void mdev_remove_sysfs_files(struct device *dev)
{
	sysfs_remove_files(&dev->kobj, mdev_host_attrs);
}
