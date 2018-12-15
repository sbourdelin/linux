// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_mailbox - Mailbox interface for Wilco Embedded Controller
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

/*
 * The Wilco EC is similar to a typical Chrome OS embedded controller.
 * It uses the same MEC based low-level communication and a similar
 * protocol, but with some important differences.  The EC firmware does
 * not support the same mailbox commands so it is not registered as a
 * cros_ec device type.
 *
 * Most messages follow a standard format, but there are some exceptions
 * and an interface is provided to do direct/raw transactions that do not
 * make assumptions about byte placement.
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/mfd/cros_ec_lpc_mec.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include "wilco_ec.h"

/* Version of mailbox interface */
#define EC_MAILBOX_VERSION		0

/* Command to start mailbox transaction */
#define EC_MAILBOX_START_COMMAND	0xda

/* Version of EC protocol */
#define EC_MAILBOX_PROTO_VERSION	3

/* Normal commands have a maximum 32 bytes of data */
#define EC_MAILBOX_DATA_SIZE		32

/* Extended commands have 256 bytes of response data */
#define EC_MAILBOX_DATA_SIZE_EXTENDED	256

/* Number of header bytes to be counted as data bytes */
#define EC_MAILBOX_DATA_EXTRA		2

/* Maximum timeout */
#define EC_MAILBOX_TIMEOUT		HZ

/* EC response flags */
#define EC_CMDR_DATA		BIT(0)	/* Data ready for host to read */
#define EC_CMDR_PENDING		BIT(1)	/* Write pending to EC */
#define EC_CMDR_BUSY		BIT(2)	/* EC is busy processing a command */
#define EC_CMDR_CMD		BIT(3)	/* Last host write was a command */

/**
 * struct wilco_ec_request - Mailbox request message format.
 * @struct_version: Should be %EC_MAILBOX_PROTO_VERSION
 * @checksum: Sum of all bytes must be 0.
 * @mailbox_id: Mailbox identifier, specifies the command set.
 * @mailbox_version: Mailbox interface version %EC_MAILBOX_VERSION
 * @reserved: Set to zero.
 * @data_size: Length of request, data + last 2 bytes of the header.
 * @command: Mailbox command code, unique for each mailbox_id set.
 * @reserved_raw: Set to zero for most commands, but is used by
 *                some command types and for raw commands.
 */
struct wilco_ec_request {
	u8 struct_version;
	u8 checksum;
	u16 mailbox_id;
	u8 mailbox_version;
	u8 reserved;
	u16 data_size;
	u8 command;
	u8 reserved_raw;
} __packed;

/**
 * struct wilco_ec_response - Mailbox response message format.
 * @struct_version: Should be %EC_MAILBOX_PROTO_VERSION
 * @checksum: Sum of all bytes must be 0.
 * @result: Result code from the EC.  Non-zero indicates an error.
 * @data_size: Length of the response data buffer.
 * @reserved: Set to zero.
 * @mbox0: EC returned data at offset 0 is unused (always 0) so this byte
 *         is treated as part of the header instead of the data.
 * @data: Response data buffer.  Max size is %EC_MAILBOX_DATA_SIZE_EXTENDED.
 */
struct wilco_ec_response {
	u8 struct_version;
	u8 checksum;
	u16 result;
	u16 data_size;
	u8 reserved[2];
	u8 mbox0;
	u8 data[0];
} __packed;

/**
 * wilco_ec_response_timed_out() - Wait for EC response.
 * @ec: EC device.
 *
 * Return: true if EC timed out, false if EC did not time out.
 */
static bool wilco_ec_response_timed_out(struct wilco_ec_device *ec)
{
	unsigned long timeout = jiffies + EC_MAILBOX_TIMEOUT;

	usleep_range(200, 300);
	do {
		if (!(inb(ec->io_command->start) &
		      (EC_CMDR_PENDING | EC_CMDR_BUSY)))
			return false;
		usleep_range(100, 200);
	} while (time_before(jiffies, timeout));

	return true;
}

/**
 * wilco_ec_checksum() - Compute 8bit checksum over data range.
 * @data: Data to checksum.
 * @size: Number of bytes to checksum.
 *
 * Return: 8bit checksum of provided data.
 */
static u8 wilco_ec_checksum(const void *data, size_t size)
{
	u8 *data_bytes = (u8 *)data;
	u8 checksum = 0;
	size_t i;

	for (i = 0; i < size; i++)
		checksum += data_bytes[i];

	return checksum;
}

/**
 * wilco_ec_prepare() - Prepare the request structure for the EC.
 * @msg: EC message with request information.
 * @rq: EC request structure to fill.
 */
static void wilco_ec_prepare(struct wilco_ec_message *msg,
			     struct wilco_ec_request *rq)
{
	memset(rq, 0, sizeof(*rq));

	/* Handle messages without trimming bytes from the request */
	if (msg->request_size && msg->flags & WILCO_EC_FLAG_RAW_REQUEST) {
		rq->reserved_raw = *(u8 *)msg->request_data;
		msg->request_size--;
		memmove(msg->request_data, msg->request_data + 1,
			msg->request_size);
	}

	/* Fill in request packet */
	rq->struct_version = EC_MAILBOX_PROTO_VERSION;
	rq->mailbox_id = msg->type;
	rq->mailbox_version = EC_MAILBOX_VERSION;
	rq->data_size = msg->request_size + EC_MAILBOX_DATA_EXTRA;
	rq->command = msg->command;

	/* Checksum header and data */
	rq->checksum = wilco_ec_checksum(rq, sizeof(*rq));
	rq->checksum += wilco_ec_checksum(msg->request_data, msg->request_size);
	rq->checksum = -rq->checksum;
}

/**
 * wilco_ec_transfer() - Send EC request and receive EC response.
 * @ec: EC device.
 * @msg: EC message data for request and response.
 *
 * Return: 0 for success or negative error code on failure.
 */
static int wilco_ec_transfer(struct wilco_ec_device *ec,
			     struct wilco_ec_message *msg)
{
	struct wilco_ec_request *rq;
	struct wilco_ec_response *rs;
	u8 checksum;
	size_t size;

	/* Prepare request packet */
	rq = ec->data_buffer;
	wilco_ec_prepare(msg, rq);

	/* Write request header */
	cros_ec_lpc_io_bytes_mec(MEC_IO_WRITE, 0, sizeof(*rq), (u8 *)rq);

	/* Write request data */
	cros_ec_lpc_io_bytes_mec(MEC_IO_WRITE, sizeof(*rq), msg->request_size,
				 msg->request_data);

	/* Start the command */
	outb(EC_MAILBOX_START_COMMAND, ec->io_command->start);

	/* Wait for it to complete */
	if (wilco_ec_response_timed_out(ec)) {
		dev_err(ec->dev, "response timed out\n");
		return -ETIMEDOUT;
	}

	/* Some commands will put the EC into a state where it cannot respond */
	if (msg->flags & WILCO_EC_FLAG_NO_RESPONSE) {
		dev_dbg(ec->dev, "EC does not respond to this command\n");
		return 0;
	}

	/* Check result */
	msg->result = inb(ec->io_data->start);
	if (msg->result) {
		dev_err(ec->dev, "bad response: 0x%02x\n", msg->result);
		return -EIO;
	}

	if (msg->flags & WILCO_EC_FLAG_EXTENDED_DATA) {
		/* Extended commands return 256 bytes of data */
		size = EC_MAILBOX_DATA_SIZE_EXTENDED;
	} else {
		/* Otherwise EC commands return 32 bytes of data */
		size = EC_MAILBOX_DATA_SIZE;
	}

	/* Read back response */
	rs = ec->data_buffer;
	checksum = cros_ec_lpc_io_bytes_mec(MEC_IO_READ, 0,
					    sizeof(*rs) + size, (u8 *)rs);
	if (checksum) {
		dev_err(ec->dev, "bad packet checksum 0x%02x\n", rs->checksum);
		return -EBADMSG;
	}
	msg->result = rs->result;

	/* Check the returned data size, skipping the header */
	if (rs->data_size != size) {
		dev_err(ec->dev, "unexpected packet size (%u != %zu)",
			rs->data_size, size);
		return -EMSGSIZE;
	}

	/* Skip 1 response data byte unless specified */
	size = (msg->flags & WILCO_EC_FLAG_RAW_RESPONSE) ? 0 : 1;

	if (msg->response_size > rs->data_size - size) {
		dev_err(ec->dev, "response data too short (%u > %zu)",
			rs->data_size - size, msg->response_size);
		return -EMSGSIZE;
	}

	/* Ignore response data bytes as requested */
	memcpy(msg->response_data, rs->data + size, msg->response_size);

	/* Return actual amount of data received */
	return msg->response_size;
}

int wilco_ec_mailbox(struct wilco_ec_device *ec, struct wilco_ec_message *msg)
{
	size_t size = EC_MAILBOX_DATA_SIZE;
	int ret;

	dev_dbg(ec->dev, "cmd=%02x type=%04x flags=%02x rslen=%d rqlen=%d\n",
		msg->command, msg->type, msg->flags, msg->response_size,
		msg->request_size);

	if (msg->request_size > size) {
		dev_err(ec->dev, "provided request too large: %zu > %zu\n",
			msg->request_size, size);
		return -EINVAL;
	}

	/* Check for extended size on response data if flag is set */
	if (msg->flags & WILCO_EC_FLAG_EXTENDED_DATA)
		size = EC_MAILBOX_DATA_SIZE_EXTENDED;

	if (msg->response_size > size) {
		dev_err(ec->dev, "expected response too large: %zu > %zu\n",
			msg->response_size, size);
		return -EINVAL;
	}
	if (msg->request_size && !msg->request_data) {
		dev_err(ec->dev, "request data missing\n");
		return -EINVAL;
	}
	if (msg->response_size && !msg->response_data) {
		dev_err(ec->dev, "response data missing\n");
		return -EINVAL;
	}

	mutex_lock(&ec->mailbox_lock);

	ret = wilco_ec_transfer(ec, msg);
	if (ret >= 0 && msg->result)
		ret = -EBADMSG;

	mutex_unlock(&ec->mailbox_lock);

	return ret;
}

static struct resource *wilco_get_resource(struct platform_device *pdev,
					   int index)
{
	struct device *dev = &pdev->dev;
	struct resource *res;

	res = platform_get_resource(pdev, IORESOURCE_IO, index);
	if (!res) {
		dev_err(dev, "couldn't find IO resource %d\n", index);
		return NULL;
	}

	if (!devm_request_region(dev, res->start, resource_size(res),
				 dev_name(dev))) {
		dev_err(dev, "couldn't reserve IO region %d\n", index);
		return NULL;
	}

	return res;
}

static int wilco_ec_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct wilco_ec_device *ec;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;
	platform_set_drvdata(pdev, ec);
	ec->dev = dev;
	mutex_init(&ec->mailbox_lock);

	/* Largest data buffer size requirement is extended data response */
	ec->data_size = sizeof(struct wilco_ec_response) +
		EC_MAILBOX_DATA_SIZE_EXTENDED;
	ec->data_buffer = devm_kzalloc(dev, ec->data_size, GFP_KERNEL);
	if (!ec->data_buffer)
		return -ENOMEM;

	/* Prepare access to IO regions provided by ACPI */
	ec->io_data = wilco_get_resource(pdev, 0);	/* Host Data */
	ec->io_command = wilco_get_resource(pdev, 1);	/* Host Command */
	ec->io_packet = wilco_get_resource(pdev, 2);	/* MEC EMI */
	if (!ec->io_data || !ec->io_command || !ec->io_packet)
		return -ENODEV;

	/* Initialize cros_ec register interface for communication */
	cros_ec_lpc_mec_init(ec->io_packet->start,
			     ec->io_packet->start + EC_MAILBOX_DATA_SIZE);

	/* Create sysfs attributes for userspace interaction */
	if (wilco_ec_sysfs_init(ec) < 0) {
		dev_err(dev, "Failed to create sysfs attributes\n");
		cros_ec_lpc_mec_destroy();
		return -ENODEV;
	}

	return 0;
}

static int wilco_ec_remove(struct platform_device *pdev)
{
	struct wilco_ec_device *ec = platform_get_drvdata(pdev);

	/* Remove sysfs attributes */
	wilco_ec_sysfs_remove(ec);

	/* Teardown cros_ec interface */
	cros_ec_lpc_mec_destroy();

	return 0;
}

static const struct acpi_device_id wilco_ec_acpi_device_ids[] = {
	{ "GOOG000C", 0 },
	{ }
};
MODULE_DEVICE_TABLE(acpi, wilco_ec_acpi_device_ids);

static struct platform_driver wilco_ec_driver = {
	.driver = {
		.name = "wilco_ec",
		.acpi_match_table = wilco_ec_acpi_device_ids,
	},
	.probe = wilco_ec_probe,
	.remove = wilco_ec_remove,
};

module_platform_driver(wilco_ec_driver);

MODULE_AUTHOR("Duncan Laurie <dlaurie@chromium.org>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Chrome OS Wilco Embedded Controller driver");
MODULE_ALIAS("platform:wilco-ec");
