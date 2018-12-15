// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_legacy - Telemetry sysfs attributes for Wilco EC
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

#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include "wilco_ec.h"
#include "wilco_ec_sysfs_util.h"

/* Data buffer for holding EC's response for telemtry data */
static u8 telemetry_data[EC_MAILBOX_DATA_SIZE_EXTENDED];

ssize_t wilco_ec_telem_write(struct file *filp, struct kobject *kobj,
			     struct bin_attribute *attr, char *buf, loff_t loff,
			     size_t count)
{
	struct wilco_ec_message msg;
	int ret;
	struct device *dev = device_from_kobject(kobj);
	struct wilco_ec_device *ec = dev_get_drvdata(dev);

	if (count < 1 || count > EC_MAILBOX_DATA_SIZE_EXTENDED)
		return -EINVAL;

	/* Clear response data buffer */
	memset(telemetry_data, 0, EC_MAILBOX_DATA_SIZE_EXTENDED);

	msg.type = WILCO_EC_MSG_TELEMETRY;
	msg.flags = WILCO_EC_FLAG_RAW | WILCO_EC_FLAG_EXTENDED_DATA;
	msg.command = buf[0];
	msg.request_data = buf + 1;
	msg.request_size = EC_MAILBOX_DATA_SIZE;
	msg.response_data = &telemetry_data;
	msg.response_size = EC_MAILBOX_DATA_SIZE_EXTENDED;

	/* Send the requested command + data as raw transaction */
	ret = wilco_ec_mailbox(ec, &msg);
	if (ret < 0)
		return ret;

	return count;
}

ssize_t wilco_ec_telem_read(struct file *filp, struct kobject *kobj,
			    struct bin_attribute *attr, char *buf, loff_t off,
			    size_t count)
{
	memcpy(buf, telemetry_data, min_t(unsigned long, count,
	       EC_MAILBOX_DATA_SIZE_EXTENDED));
	return count;
}
