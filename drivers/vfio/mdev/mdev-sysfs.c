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

#define UUID_CHAR_LENGTH	36
#define UUID_BYTE_LENGTH	16

#define SUPPORTED_TYPE_BUFFER_LENGTH	1024

static inline bool is_uuid_sep(char sep)
{
	if (sep == '\n' || sep == '-' || sep == ':' || sep == '\0')
		return true;
	return false;
}

static int uuid_parse(const char *str, uuid_le *uuid)
{
	int i;

	if (strlen(str) < UUID_CHAR_LENGTH)
		return -1;

	for (i = 0; i < UUID_BYTE_LENGTH; i++) {
		if (!isxdigit(str[0]) || !isxdigit(str[1])) {
			pr_err("%s err", __func__);
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
static ssize_t mdev_supported_types_show(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	char *str;
	ssize_t n;

	str = kzalloc(sizeof(*str) * SUPPORTED_TYPE_BUFFER_LENGTH, GFP_KERNEL);
	if (!str)
		return -ENOMEM;

	get_mdev_supported_types(dev, str);

	n = sprintf(buf, "%s\n", str);
	kfree(str);

	return n;
}

static ssize_t mdev_create_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	char *str, *pstr;
	char *uuid_str, *instance_str, *mdev_params = NULL;
	uuid_le uuid;
	uint32_t instance;
	int ret = 0;

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

	if (!str) {
		pr_err("mdev_create: mdev params not specified %s\n", buf);
		ret = -EINVAL;
		goto create_error;
	}

	mdev_params = kstrdup(str, GFP_KERNEL);

	if (!mdev_params) {
		ret = -EINVAL;
		goto create_error;
	}

	if (uuid_parse(uuid_str, &uuid) < 0) {
		pr_err("mdev_create: UUID parse error %s\n", buf);
		ret = -EINVAL;
		goto create_error;
	}

	if (create_mdev_device(dev, uuid, instance, mdev_params) < 0) {
		pr_err("mdev_create: Failed to create mdev device\n");
		ret = -EINVAL;
		goto create_error;
	}
	ret = count;

create_error:
	kfree(mdev_params);
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

	if (uuid_parse(uuid_str, &uuid) < 0) {
		pr_err("mdev_destroy: UUID parse error  %s\n", buf);
		ret = -EINVAL;
		goto destroy_error;
	}

	ret = destroy_mdev_device(uuid, instance);
	if (ret < 0)
		goto destroy_error;

	ret = count;

destroy_error:
	kfree(pstr);
	return ret;
}

ssize_t mdev_start_store(struct class *class, struct class_attribute *attr,
			 const char *buf, size_t count)
{
	char *uuid_str;
	uuid_le uuid;
	int ret = 0;

	uuid_str = kstrndup(buf, count, GFP_KERNEL);

	if (!uuid_str)
		return -ENOMEM;

	if (uuid_parse(uuid_str, &uuid) < 0) {
		pr_err("mdev_start: UUID parse error  %s\n", buf);
		ret = -EINVAL;
		goto start_error;
	}

	ret = mdev_start_callback(uuid, 0);
	if (ret < 0)
		goto start_error;

	ret = count;

start_error:
	kfree(uuid_str);
	return ret;
}

ssize_t mdev_shutdown_store(struct class *class, struct class_attribute *attr,
			    const char *buf, size_t count)
{
	char *uuid_str;
	uuid_le uuid;
	int ret = 0;

	uuid_str = kstrndup(buf, count, GFP_KERNEL);

	if (!uuid_str)
		return -ENOMEM;

	if (uuid_parse(uuid_str, &uuid) < 0) {
		pr_err("mdev_shutdown: UUID parse error %s\n", buf);
		ret = -EINVAL;
	}

	ret = mdev_shutdown_callback(uuid, 0);
	if (ret < 0)
		goto shutdown_error;

	ret = count;

shutdown_error:
	kfree(uuid_str);
	return ret;

}

struct class_attribute mdev_class_attrs[] = {
	__ATTR_WO(mdev_start),
	__ATTR_WO(mdev_shutdown),
	__ATTR_NULL
};

int mdev_create_sysfs_files(struct device *dev)
{
	int retval;

	retval = sysfs_create_file(&dev->kobj,
				   &dev_attr_mdev_supported_types.attr);
	if (retval) {
		pr_err("Failed to create mdev_supported_types sysfs entry\n");
		return retval;
	}

	retval = sysfs_create_file(&dev->kobj, &dev_attr_mdev_create.attr);
	if (retval) {
		pr_err("Failed to create mdev_create sysfs entry\n");
		return retval;
	}

	retval = sysfs_create_file(&dev->kobj, &dev_attr_mdev_destroy.attr);
	if (retval) {
		pr_err("Failed to create mdev_destroy sysfs entry\n");
		return retval;
	}

	return 0;
}

void mdev_remove_sysfs_files(struct device *dev)
{
	sysfs_remove_file(&dev->kobj, &dev_attr_mdev_supported_types.attr);
	sysfs_remove_file(&dev->kobj, &dev_attr_mdev_create.attr);
	sysfs_remove_file(&dev->kobj, &dev_attr_mdev_destroy.attr);
}
