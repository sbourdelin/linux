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

#ifdef CONFIG_WILCO_EC_SYSFS_RAW

/* Raw data buffer, large enough to hold extended responses */
static size_t raw_response_size;
static u8 raw_response_data[EC_MAILBOX_DATA_SIZE_EXTENDED];

/*
 * raw: write a raw command and return the result
 *
 * Bytes 0-1 indicate the message type:
 *  00 F0 = Execute Legacy Command
 *  00 F2 = Read/Write NVRAM Property
 * Byte 2 provides the command code
 * Bytes 3+ consist of the data passed in the request
 *
 * example: read the EC info type 1:
 *  # echo 00 f0 38 00 01 00 > raw
 *  # cat raw
 *  00 38 31 34 34 66 00 00 00 00 00 00 00 00 00 00 00...
 */

static ssize_t raw_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	ssize_t count = 0;

	if (raw_response_size) {
		int i;

		for (i = 0; i < raw_response_size; ++i)
			count += scnprintf(buf + count, PAGE_SIZE - count,
					   "%02x ", raw_response_data[i]);

		count += scnprintf(buf + count, PAGE_SIZE - count, "\n");

		/* Only return response the first time it is read */
		raw_response_size = 0;
	}

	return count;
}

static ssize_t raw_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct wilco_ec_device *ec = dev_get_drvdata(dev);
	struct wilco_ec_message msg;
	u8 raw_request_data[EC_MAILBOX_DATA_SIZE];
	int in_offset = 0;
	int out_offset = 0;
	int ret;

	while (in_offset < count) {
		char word_buf[EC_MAILBOX_DATA_SIZE];
		u8 byte;
		int start_offset = in_offset;
		int end_offset;

		/* Find the start of the byte */
		while (buf[start_offset] && isspace(buf[start_offset]))
			start_offset++;
		if (!buf[start_offset])
			break;

		/* Find the start of the next byte, if any */
		end_offset = start_offset;
		while (buf[end_offset] && !isspace(buf[end_offset]))
			end_offset++;
		if (start_offset > count || end_offset > count)
			break;
		if (start_offset > EC_MAILBOX_DATA_SIZE ||
		    end_offset > EC_MAILBOX_DATA_SIZE)
			break;

		/* Copy to a new nul-terminated string */
		memcpy(word_buf, buf + start_offset, end_offset - start_offset);
		word_buf[end_offset - start_offset] = '\0';

		/* Convert from hex string */
		ret = kstrtou8(word_buf, 16, &byte);
		if (ret)
			break;

		/* Fill this byte into the request buffer */
		raw_request_data[out_offset++] = byte;
		if (out_offset >= EC_MAILBOX_DATA_SIZE)
			break;

		in_offset = end_offset;
	}
	if (out_offset == 0)
		return -EINVAL;

	/* Clear response data buffer */
	memset(raw_response_data, 0, EC_MAILBOX_DATA_SIZE_EXTENDED);

	msg.type = raw_request_data[0] << 8 | raw_request_data[1];
	msg.flags = WILCO_EC_FLAG_RAW;
	msg.command = raw_request_data[2];
	msg.request_data = raw_request_data + 3;
	msg.request_size = out_offset - 3;
	msg.response_data = raw_response_data;
	msg.response_size = EC_MAILBOX_DATA_SIZE;

	/* Telemetry commands use extended response data */
	if (msg.type == WILCO_EC_MSG_TELEMETRY) {
		msg.flags |= WILCO_EC_FLAG_EXTENDED_DATA;
		msg.response_size = EC_MAILBOX_DATA_SIZE_EXTENDED;
	}

	ret = wilco_ec_mailbox(ec, &msg);
	if (ret < 0)
		return ret;
	raw_response_size = ret;
	return count;
}

#endif /* CONFIG_WILCO_EC_SYSFS_RAW */

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
#ifdef CONFIG_WILCO_EC_SYSFS_RAW
static DEVICE_ATTR_RW(raw);
#endif

static struct attribute *wilco_ec_attrs[] = {
	&dev_attr_version.attr,
	&dev_attr_stealth_mode.attr,
#ifdef CONFIG_WILCO_EC_SYSFS_RAW
	&dev_attr_raw.attr,
#endif
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
