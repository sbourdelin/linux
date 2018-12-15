// SPDX-License-Identifier: GPL-2.0
/*
 * wilco_ec_properties - set/get properties of Wilco Embedded Controller
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

#include <linux/device.h>
#include "wilco_ec_properties.h"
#include "wilco_ec.h"
#include "wilco_ec_sysfs_util.h"

/* Payload length for get/set properties */
#define PROPERTY_DATA_MAX_LENGTH 4

struct ec_property_get_request {
	u32 property_id;
	u8 length;
} __packed;

struct ec_property_set_request {
	u32 property_id;
	u8 length;
	u8 data[PROPERTY_DATA_MAX_LENGTH];
} __packed;

struct ec_property_response {
	u8 status;
	u8 sub_function;
	u32 property_id;
	u8 length;
	u8 data[PROPERTY_DATA_MAX_LENGTH];
} __packed;

/* Store a 32 bit property ID into an array or a field in a struct, LSB first */
static inline void fill_property_id(u32 property_id, u8 field[])
{
	field[0] =  property_id        & 0xff;
	field[1] = (property_id >> 8)  & 0xff;
	field[2] = (property_id >> 16) & 0xff;
	field[3] = (property_id >> 24) & 0xff;
}

/* Extract 32 bit property ID from an array or a field in a struct, LSB first */
static inline u32 extract_property_id(u8 field[])
{
	return (uint32_t)field[0]	|
	       (uint32_t)field[1] << 8  |
	       (uint32_t)field[2] << 16 |
	       (uint32_t)field[3] << 24;
}

/**
 * check_property_response() - Verify that the response from the EC is valid.
 * @ec: EC device
 * @rs: bytes sent back from the EC, filled into struct
 * @op: Which of [SET, GET, SYNC] we are responding to
 * @expected_property_id: Property ID that we were trying to read
 * @expected_length: Number of bytes of actual payload we expected
 * @expected_data: What we expect the EC to echo back for a SET. For GETting
 *		   or SYNCing, we don't know the response, so use NULL to ignore
 *
 * Return: 0 on success, -EBADMSG on failure.
 */
static int check_property_response(struct wilco_ec_device *ec,
				   struct ec_property_response *rs,
				   enum get_set_sync_op op,
				   u32 expected_property_id, u8 expected_length,
				   const u8 expected_data[])
{
	u32 received_property_id;
	int i;

	/* check for success/failure flag */
	if (rs->status) {
		dev_err(ec->dev, "EC reports failure to get property");
		return -EBADMSG;
	}

	/* Which subcommand is the EC responding to? */
	if (rs->sub_function != op) {
		dev_err(ec->dev, "For SET/GET/SYNC, EC replied %d, expected %d",
			rs->sub_function, op);
		return -EBADMSG;
	}

	/* Check that returned property_id is what we expect */
	received_property_id = extract_property_id((u8 *)&rs->property_id);
	if (received_property_id != expected_property_id) {
		dev_err(ec->dev,
			"EC responded to property_id 0x%08x, expected 0x%08x",
			received_property_id, expected_property_id);
		return -EBADMSG;
	}

	/* Did we get the correct number of bytes as a payload? */
	if (rs->length != expected_length) {
		dev_err(ec->dev, "EC returned %d bytes when we expected %d",
			rs->length, expected_length);
		return -EBADMSG;
	}

	/* Check that the actual data returned was what we expected */
	if (expected_length < 1 || !expected_data)
		return 0;
	for (i = 0; i < expected_length; i++) {
		if (rs->data[i] != expected_data[i]) {
			dev_err(ec->dev, "returned[%d]=%2x != expected[%d]=%2x",
				i, rs->data[i], i, expected_data[i]);
			return -EBADMSG;
		}
	}

	return 0;
}

static inline int check_get_property_response(struct wilco_ec_device *ec,
					      struct ec_property_response *rs,
					      u32 expected_property_id,
					      u8 expected_length)
{
	return check_property_response(ec, rs, OP_GET, expected_property_id,
				       expected_length, NULL);
}

static inline int check_set_property_response(struct wilco_ec_device *ec,
					      struct ec_property_response *rs,
					      enum get_set_sync_op op,
					      u32 expected_property_id,
					      u8 expected_length,
					      const u8 expected_data[])
{
	return check_property_response(ec, rs, op, expected_property_id,
				       expected_length, expected_data);
}

ssize_t wilco_ec_get_property(struct wilco_ec_device *ec, u32 property_id,
			      u8 result_length, u8 *result)
{
	int ret, response_valid;
	struct ec_property_get_request rq;
	struct ec_property_response rs;
	struct wilco_ec_message msg = {
		.type = WILCO_EC_MSG_PROPERTY,
		.flags = WILCO_EC_FLAG_RAW,
		.command = OP_GET,
		.request_data = &rq,
		.request_size = sizeof(rq),
		.response_data = &rs,
		.response_size = sizeof(rs),
	};

	/* Create the request struct */
	if (result_length < 1) {
		dev_err(ec->dev,
			"Requested %d bytes when getting property, min is 0\n",
			result_length);
		return -EINVAL;
	}
	if (result_length > PROPERTY_DATA_MAX_LENGTH) {
		dev_err(ec->dev,
			"Requested %d bytes when getting property, max is %d\n",
			result_length, PROPERTY_DATA_MAX_LENGTH);
		return -EINVAL;
	}
	fill_property_id(property_id, (u8 *)&(rq.property_id));
	rq.length = 0;

	/* send and receive */
	ret = wilco_ec_mailbox(ec, &msg);
	if (ret < 0) {
		dev_err(ec->dev, "Get Property 0x%08x command failed\n",
			property_id);
		return ret;
	}

	/* verify that the response was valid */
	response_valid = check_get_property_response(ec, &rs, property_id,
						     result_length);
	if (response_valid < 0)
		return response_valid;

	memcpy(result, &rs.data, result_length);
	return ret;
}

ssize_t wilco_ec_set_property(struct wilco_ec_device *ec,
			      enum get_set_sync_op op, u32 property_id,
			      u8 length, const u8 *data)
{
	int ret;
	struct ec_property_set_request rq;
	struct ec_property_response rs;
	u8 request_length = sizeof(rq) - PROPERTY_DATA_MAX_LENGTH + length;
	struct wilco_ec_message msg = {
		.type = WILCO_EC_MSG_PROPERTY,
		.flags = WILCO_EC_FLAG_RAW,
		.command = op,
		.request_data = &rq,
		.request_size = request_length,
		.response_data = &rs,
		.response_size = sizeof(rs),
	};

	/* make request */
	if (op != OP_SET && op != OP_SYNC) {
		dev_err(ec->dev, "Set op must be OP_SET | OP_SYNC, got %d", op);
		return -EINVAL;
	}
	if (length < 1) {
		dev_err(ec->dev,
			"Sending %d bytes when setting property, min is 1",
			length);
		return -EINVAL;
	}
	if (length > PROPERTY_DATA_MAX_LENGTH) {
		dev_err(ec->dev,
			"Sending %d bytes when setting property, max is %d",
			length, PROPERTY_DATA_MAX_LENGTH);
		return -EINVAL;
	}
	fill_property_id(property_id, (u8 *)&(rq.property_id));
	rq.length = length;
	memcpy(rq.data, data, length);

	/* send and receive */
	ret = wilco_ec_mailbox(ec, &msg);
	if (ret < 0) {
		dev_err(ec->dev, "Set Property 0x%08x command failed\n",
			property_id);
		return ret;
	}

	/* verify that the response was valid, EC echoing back stored value */
	ret = check_set_property_response(ec, &rs, op, property_id,
						     length, data);
	if (ret < 0)
		return ret;

	return 0;
}

ssize_t wilco_ec_get_bool_prop(struct device *dev, u32 property_id,
			       char *result)
{
	struct wilco_ec_device *ec = dev_get_drvdata(dev);
	int ret;

	ret = wilco_ec_get_property(ec, property_id, 1, result);
	if (ret < 0)
		return ret;

	/* convert the raw byte response into ascii */
	switch (result[0]) {
	case 0:
		result[0] = '0';
		break;
	case 1:
		result[0] = '1';
		break;
	default:
		dev_err(ec->dev, "Expected 0 or 1 as response, got %02x",
			result[0]);
		return -EBADMSG;
	}

	/* Tack on a newline */
	result[1] = '\n';
	return 2;
}

ssize_t wilco_ec_set_bool_prop(struct device *dev, enum get_set_sync_op op,
			       u32 property_id, const char *buf, size_t count)
{
	struct wilco_ec_device *ec = dev_get_drvdata(dev);
	bool enable;
	u8 param;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret) {
		dev_err(dev, "Unable to parse '%s' to a bool", buf);
		return ret;
	}
	param = enable;

	ret = wilco_ec_set_property(ec, op, property_id, 1, &param);
	if (ret < 0)
		return ret;

	return count;
}

ssize_t wilco_ec_bool_prop_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct property_attribute *prop_attr;
	struct device *dev;

	prop_attr = container_of(attr, struct property_attribute, kobj_attr);
	dev = device_from_kobject(kobj);

	return wilco_ec_get_bool_prop(dev, prop_attr->pid, buf);
}

ssize_t wilco_ec_bool_prop_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count)
{
	struct property_attribute *prop_attr;
	struct device *dev;

	prop_attr = container_of(attr, struct property_attribute, kobj_attr);
	dev = device_from_kobject(kobj);

	return wilco_ec_set_bool_prop(dev, prop_attr->op, prop_attr->pid, buf,
				      count);
}
