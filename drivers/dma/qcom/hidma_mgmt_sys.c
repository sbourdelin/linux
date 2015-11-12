/*
 * Qualcomm Technologies HIDMA Management SYS interface
 *
 * Copyright (c) 2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/sysfs.h>
#include <linux/platform_device.h>

#include "hidma_mgmt.h"

struct fileinfo {
	char *name;
	int mode;
	int (*get)(struct hidma_mgmt_dev *mdev);
	int (*set)(struct hidma_mgmt_dev *mdev, u64 val);
};

#define IMPLEMENT_GETSET(name)					\
static int get_##name(struct hidma_mgmt_dev *mdev)		\
{								\
	return mdev->name;					\
}								\
static int set_##name(struct hidma_mgmt_dev *mdev, u64 val)	\
{								\
	u64 tmp;						\
	int rc;							\
								\
	tmp = mdev->name;					\
	mdev->name = val;					\
	rc = hidma_mgmt_setup(mdev);				\
	if (rc)							\
		mdev->name = tmp;				\
	return rc;						\
}

#define DECLARE_ATTRIBUTE(name, mode)				\
	{#name, mode, get_##name, set_##name}

IMPLEMENT_GETSET(hw_version_major)
IMPLEMENT_GETSET(hw_version_minor)
IMPLEMENT_GETSET(max_wr_xactions)
IMPLEMENT_GETSET(max_rd_xactions)
IMPLEMENT_GETSET(max_write_request)
IMPLEMENT_GETSET(max_read_request)
IMPLEMENT_GETSET(dma_channels)
IMPLEMENT_GETSET(chreset_timeout_cycles)

static int set_priority(struct hidma_mgmt_dev *mdev, unsigned int i, u64 val)
{
	u64 tmp;
	int rc;

	if (i > mdev->dma_channels)
		return -EINVAL;

	tmp = mdev->priority[i];
	mdev->priority[i] = val;
	rc = hidma_mgmt_setup(mdev);
	if (rc)
		mdev->priority[i] = tmp;
	return rc;
}

static int set_weight(struct hidma_mgmt_dev *mdev, unsigned int i, u64 val)
{
	u64 tmp;
	int rc;

	if (i > mdev->dma_channels)
		return -EINVAL;

	tmp = mdev->weight[i];
	mdev->weight[i] = val;
	rc = hidma_mgmt_setup(mdev);
	if (rc)
		mdev->weight[i] = tmp;
	return rc;
}

static struct fileinfo files[] = {
	DECLARE_ATTRIBUTE(hw_version_major, S_IRUGO),
	DECLARE_ATTRIBUTE(hw_version_minor, S_IRUGO),
	DECLARE_ATTRIBUTE(dma_channels, S_IRUGO),
	DECLARE_ATTRIBUTE(chreset_timeout_cycles, S_IRUGO),
	DECLARE_ATTRIBUTE(max_wr_xactions, (S_IRUGO|S_IWUGO)),
	DECLARE_ATTRIBUTE(max_rd_xactions, (S_IRUGO|S_IWUGO)),
	DECLARE_ATTRIBUTE(max_write_request, (S_IRUGO|S_IWUGO)),
	DECLARE_ATTRIBUTE(max_read_request, (S_IRUGO|S_IWUGO)),
};

static ssize_t show_values(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct hidma_mgmt_dev *mdev = platform_get_drvdata(pdev);
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		if (strcmp(attr->attr.name, files[i].name) == 0) {
			sprintf(buf, "%d\n", files[i].get(mdev));
			goto done;
		}
	}

	for (i = 0; i < mdev->dma_channels; i++) {
		char name[30];

		sprintf(name, "channel%d_priority", i);
		if (strcmp(attr->attr.name, name) == 0) {
			sprintf(buf, "%d\n", mdev->priority[i]);
			goto done;
		}

		sprintf(name, "channel%d_weight", i);
		if (strcmp(attr->attr.name, name) == 0) {
			sprintf(buf, "%d\n", mdev->weight[i]);
			goto done;
		}
	}

done:
	return strlen(buf);
}

static ssize_t set_values(struct device *dev, struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct hidma_mgmt_dev *mdev = platform_get_drvdata(pdev);
	unsigned long tmp;
	unsigned int i;
	int rc;

	rc = kstrtoul(buf, 0, &tmp);
	if (rc)
		return rc;

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		if (strcmp(attr->attr.name, files[i].name) == 0) {
			rc = files[i].set(mdev, tmp);
			if (rc)
				return rc;

			goto done;
		}
	}

	for (i = 0; i < mdev->dma_channels; i++) {
		char name[30];

		sprintf(name, "channel%d_priority", i);
		if (strcmp(attr->attr.name, name) == 0) {
			rc = set_priority(mdev, i, tmp);
			if (rc)
				return rc;
			goto done;
		}

		sprintf(name, "channel%d_weight", i);
		if (strcmp(attr->attr.name, name) == 0) {
			rc = set_weight(mdev, i, tmp);
			if (rc)
				return rc;
		}
	}
done:
	return count;
}

static int create_sysfs_entry(struct hidma_mgmt_dev *dev, char *name, int mode)
{
	struct device_attribute *port_attrs;
	char *name_copy;

	port_attrs = devm_kmalloc(&dev->pdev->dev,
			sizeof(struct device_attribute), GFP_KERNEL);
	if (!port_attrs)
		return -ENOMEM;

	name_copy = devm_kstrdup(&dev->pdev->dev, name, GFP_KERNEL);
	if (!name_copy)
		return -ENOMEM;

	port_attrs->attr.name = name_copy;
	port_attrs->attr.mode = mode;
	port_attrs->show      = show_values;
	port_attrs->store     = set_values;
	sysfs_attr_init(&port_attrs->attr);

	return device_create_file(&dev->pdev->dev, port_attrs);
}


int hidma_mgmt_init_sys(struct hidma_mgmt_dev *dev)
{
	unsigned int i;
	int rc;

	for (i = 0; i < ARRAY_SIZE(files); i++) {
		rc = create_sysfs_entry(dev, files[i].name, files[i].mode);
		if (rc)
			return rc;
	}

	for (i = 0; i < dev->dma_channels; i++) {
		char name[30];

		sprintf(name, "channel%d_priority", i);
		rc = create_sysfs_entry(dev, name, (S_IRUGO|S_IWUGO));
		if (rc)
			return rc;

		sprintf(name, "channel%d_weight", i);
		rc = create_sysfs_entry(dev, name, (S_IRUGO|S_IWUGO));
		if (rc)
			return rc;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(hidma_mgmt_init_sys);
