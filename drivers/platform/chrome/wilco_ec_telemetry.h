/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wilco_ec_telemetry - Telemetry sysfs attributes for Wilco EC
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

#ifndef WILCO_EC_TELEMETRY_H
#define WILCO_EC_TELEMETRY_H

#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include "wilco_ec.h"

ssize_t wilco_ec_telem_write(struct file *filp, struct kobject *kobj,
			     struct bin_attribute *attr, char *buf, loff_t loff,
			     size_t count);

ssize_t wilco_ec_telem_read(struct file *filp, struct kobject *kobj,
			    struct bin_attribute *attr, char *buf, loff_t off,
			    size_t count);

#define TELEMETRY_BIN_ATTR(_name) {					\
	.attr = {.name = __stringify(_name),				\
		 .mode = VERIFY_OCTAL_PERMISSIONS(0644) },		\
	.size = EC_MAILBOX_DATA_SIZE_EXTENDED,				\
	.read = wilco_ec_telem_read,					\
	.write = wilco_ec_telem_write,					\
}

#endif /* WILCO_EC_TELEMETRY_H */
