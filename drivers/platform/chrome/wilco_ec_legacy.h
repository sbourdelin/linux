/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wilco_ec_legacy - Legacy (non-Chrome-specific) sysfs attributes for Wilco EC
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

#ifndef WILCO_EC_LEGACY_H
#define WILCO_EC_LEGACY_H

#include <linux/device.h>

#define EC_COMMAND_EC_INFO		0x38
#define EC_INFO_SIZE			 9
#define EC_COMMAND_STEALTH_MODE		0xfc

#ifdef CONFIG_WILCO_EC_SYSFS_RAW

/**
 * raw_store() - Write a raw command to EC, store the result to view later
 * @dev: Device representing the EC
 * @attr: The attribute in question
 * @buf: Input buffer, format explained below
 * @count: Number of bytes in input buffer
 *
 * Bytes 0-1 indicate the message type:
 *  00 F0 = Execute Legacy Command
 *  00 F2 = Read/Write NVRAM Property
 * Byte 2 provides the command code
 * Bytes 3+ consist of the data passed in the request
 *
 * example: read the EC info type 1:
 *  # echo 00 f0 38 00 01 00 > raw
 *
 * After calling this function, read the result by using raw_show()
 *
 * Return: Number of bytes consumed from input, negative error code on failure
 */
ssize_t wilco_ec_raw_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count);

/**
 * raw_show() - Show result from previous call to raw_store()
 * @dev: Device representing the EC
 * @attr: The attribute in question
 * @buf: Output buffer to be filled
 *
 * Example usage:
 *	// Call raw_store(), read EC info type 1
 *	# echo 00 f0 38 00 01 00 > raw
 *	// Call this function, view the result
 *	# cat raw
 *	00 38 31 34 34 66 00 00 00 00 00 00 00 00 00 00 00...
 *
 * Return: Number of bytes written to output, negative error code on failure
 */
ssize_t wilco_ec_raw_show(struct device *dev, struct device_attribute *attr,
			  char *buf);

#endif /* CONFIG_WILCO_EC_SYSFS_RAW */

/**
 * version_show() - Display Wilco Embedded Controller version info
 *
 * Output will be similar to the example below:
 * Label        : 95.00.06
 * SVN Revision : 5960a.06
 * Model Number : 08;8
 * Build Date   : 11/29/18
 */
ssize_t wilco_ec_version_show(struct device *dev, struct device_attribute *attr,
			      char *buf);
/**
 * stealth_mode_store() - Turn stealth_mode on or off on EC
 * @dev: Device representing the EC
 * @attr: The attribute in question
 * @buf: Input buffer, should be parseable by kstrtobool(). Anything parsed to
 *	 True means enable stealth mode (turn off screen, etc)
 * @count: Number of bytes in input buffer
 *
 * Return: Number of bytes consumed from input, negative error code on failure
 */
ssize_t wilco_ec_stealth_mode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count);

#endif /* WILCO_EC_LEGACY_H */
