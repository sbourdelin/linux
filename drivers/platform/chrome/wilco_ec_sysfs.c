// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_sysfs - Sysfs attributes for Wilco Embedded Controller
 *
 * Copyright 2018 Google LLC
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/ctype.h>
#include <linux/platform_device.h>
#include <linux/sysfs.h>
#include "wilco_ec.h"

#define EC_COMMAND_EC_INFO		0x38
#define EC_INFO_SIZE			 9
#define EC_COMMAND_STEALTH_MODE		0xfc

struct ec_info {
	u8 index;
	const char *label;
};

static ssize_t wilco_ec_show_info(struct wilco_ec_device *ec, char *buf,
				  ssize_t count, struct ec_info *info)
{
	char result[EC_INFO_SIZE];
	struct wilco_ec_message msg = {
		.type = WILCO_EC_MSG_LEGACY,
		.command = EC_COMMAND_EC_INFO,
		.request_data = &info->index,
		.request_size = sizeof(info->index),
		.response_data = result,
		.response_size = EC_INFO_SIZE,
	};
	int ret;

	ret = wilco_ec_mailbox(ec, &msg);
	if (ret != EC_INFO_SIZE)
		return scnprintf(buf + count, PAGE_SIZE - count,
				 "%-12s : ERROR %d\n", info->label, ret);

	return scnprintf(buf + count, PAGE_SIZE - count,
			 "%-12s : %s\n", info->label, result);
}

static ssize_t version_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct wilco_ec_device *ec = dev_get_drvdata(dev);
	struct ec_info wilco_ec_info[] = {
		{ 0, "Label" },
		{ 1, "SVN Revision" },
		{ 2, "Model Number" },
		{ 3, "Build Date" },
		{ 0xff, NULL },
	};
	struct ec_info *info = wilco_ec_info;
	ssize_t c = 0;

	for (info = wilco_ec_info; info->label; info++)
		c += wilco_ec_show_info(ec, buf, c, info);

	return c;
}

static ssize_t stealth_mode_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct wilco_ec_device *ec = dev_get_drvdata(dev);
	u8 param;
	struct wilco_ec_message msg = {
		.type = WILCO_EC_MSG_LEGACY,
		.command = EC_COMMAND_STEALTH_MODE,
		.request_data = &param,
		.request_size = sizeof(param),
	};
	int ret;
	bool enable;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	/* Invert input parameter, EC expects 0=on and 1=off */
	param = enable ? 0 : 1;

	ret = wilco_ec_mailbox(ec, &msg);
	if (ret < 0)
		return ret;

	return count;
}

static DEVICE_ATTR_RO(version);
static DEVICE_ATTR_WO(stealth_mode);

static struct attribute *wilco_ec_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_stealth_mode.attr,
	NULL
};
ATTRIBUTE_GROUPS(wilco_ec);

int wilco_ec_sysfs_init(struct wilco_ec_device *ec)
{
	return sysfs_create_groups(&ec->dev->kobj, wilco_ec_groups);
}

void wilco_ec_sysfs_remove(struct wilco_ec_device *ec)
{
	sysfs_remove_groups(&ec->dev->kobj, wilco_ec_groups);
}
