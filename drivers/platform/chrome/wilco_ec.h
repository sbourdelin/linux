/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wilco_ec - Chrome OS Wilco Embedded Controller
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

#ifndef WILCO_EC_H
#define WILCO_EC_H

#include <linux/device.h>
#include <linux/kernel.h>

#define WILCO_EC_FLAG_NO_RESPONSE	BIT(0) /* EC does not respond */
#define WILCO_EC_FLAG_EXTENDED_DATA	BIT(1) /* EC returns 256 data bytes */
#define WILCO_EC_FLAG_RAW_REQUEST	BIT(2) /* Do not trim request data */
#define WILCO_EC_FLAG_RAW_RESPONSE	BIT(3) /* Do not trim response data */
#define WILCO_EC_FLAG_RAW		(WILCO_EC_FLAG_RAW_REQUEST | \
					 WILCO_EC_FLAG_RAW_RESPONSE)

/**
 * enum wilco_ec_msg_type - Message type to select a set of command codes.
 * @WILCO_EC_MSG_LEGACY: Legacy EC messages for standard EC behavior.
 * @WILCO_EC_MSG_PROPERTY: Get/Set/Sync EC controlled NVRAM property.
 * @WILCO_EC_MSG_TELEMETRY: Telemetry data provided by the EC.
 */
enum wilco_ec_msg_type {
	WILCO_EC_MSG_LEGACY = 0x00f0,
	WILCO_EC_MSG_PROPERTY = 0x00f2,
	WILCO_EC_MSG_TELEMETRY = 0x00f5,
};

/**
 * struct wilco_ec_device - Wilco Embedded Controller handle.
 * @dev: Device handle.
 * @mailbox_lock: Mutex to ensure one mailbox command at a time.
 * @io_command: I/O port for mailbox command.  Provided by ACPI.
 * @io_data: I/O port for mailbox data.  Provided by ACPI.
 * @io_packet: I/O port for mailbox packet data.  Provided by ACPI.
 * @data_buffer: Buffer used for EC communication.  The same buffer
 *               is used to hold the request and the response.
 * @data_size: Size of the data buffer used for EC communication.
 */
struct wilco_ec_device {
	struct device *dev;
	struct mutex mailbox_lock;
	struct resource *io_command;
	struct resource *io_data;
	struct resource *io_packet;
	void *data_buffer;
	size_t data_size;
};

/**
 * struct wilco_ec_message - Request and response message.
 * @type: Mailbox message type.
 * @flags: Message flags.
 * @command: Mailbox command code.
 * @result: Result code from the EC.  Non-zero indicates an error.
 * @request_size: Number of bytes to send to the EC.
 * @request_data: Buffer containing the request data.
 * @response_size: Number of bytes expected from the EC.
 *                 This is 32 by default and 256 if the flag
 *                 is set for %WILCO_EC_FLAG_EXTENDED_DATA
 * @response_data: Buffer containing the response data, should be
 *                 response_size bytes and allocated by caller.
 */
struct wilco_ec_message {
	enum wilco_ec_msg_type type;
	u8 flags;
	u8 command;
	u8 result;
	size_t request_size;
	void *request_data;
	size_t response_size;
	void *response_data;
};

/**
 * wilco_ec_mailbox() - Send request to the EC and receive the response.
 * @ec: Wilco EC device.
 * @msg: Wilco EC message.
 *
 * Return: Number of bytes received or negative error code on failure.
 */
int wilco_ec_mailbox(struct wilco_ec_device *ec, struct wilco_ec_message *msg);

#endif /* WILCO_EC_H */
