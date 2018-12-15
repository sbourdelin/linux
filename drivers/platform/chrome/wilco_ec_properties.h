/* SPDX-License-Identifier: GPL-2.0 */
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

#ifndef WILCO_EC_PROPERTIES_H
#define WILCO_EC_PROPERTIES_H

#include <linux/ctype.h>
#include <linux/kobject.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include "wilco_ec.h"

#define PID_GLOBAL_MIC_MUTE_LED			0x0676
#define PID_FN_LOCK				0x067b
#define PID_NIC					0x04ea
#define PID_EXT_USB_PORT_EN			0x0612
#define PID_WIRELESS_SW_WLAN			0x0620
#define PID_AUTO_BOOT_ON_TRINITY_DOCK_ATTACH	0x0725
#define PID_ICH_AZALIA_EN			0x0a07
#define PID_SIGN_OF_LIFE_KBBL			0x058f

/**
 * enum get_set_sync_op - three different subcommands for WILCO_EC_MSG_PROPERTY.
 *
 * OP_GET requests the property from the EC. OP_SET and OP_SYNC do the exact
 * same thing from our perspective: save a property. Only one of them works for
 * a given property, so each property uses either OP_GET and OP_SET, or
 * OP_GET and OP_SYNC
 */
enum get_set_sync_op {
	OP_GET = 0,
	OP_SET = 1,
	OP_SYNC = 4
};

/**
 * struct property_attribute - A attribute representing an EC property
 * @kobj_attr: The underlying kobj_attr that is registered with sysfs
 * @pid: Property ID of this property
 * @op: Either OP_SET or OP_SYNC, whichever this property uses
 */
struct property_attribute {
	struct kobj_attribute kobj_attr;
	u32 pid;
	enum get_set_sync_op op;
};

/**
 * wilco_ec_get_property() - Query a property from the EC
 * @ec: EC device to query
 * @property_id: Property ID
 * @result_length: Number of bytes expected in result
 * @result: Destination buffer for result, needs to be able to hold at least
 *	    @result_length bytes
 *
 * Return: Number of bytes received from EC (AKA @result_length),
 *	   negative error code on failure.
 */
ssize_t wilco_ec_get_property(struct wilco_ec_device *ec, u32 property_id,
			      u8 result_length, u8 *result);

/**
 * wilco_ec_set_property() - Set a property on EC
 * @ec: EC device to use
 * @op: either OP_SET or OP_SYNC
 * @property_id: Property ID
 * @length: Number of bytes in input buffer @data
 * @data: Input buffer
 *
 * Return: 0 on success, negative error code on failure
 */
ssize_t wilco_ec_set_property(struct wilco_ec_device *ec,
			      enum get_set_sync_op op, u32 property_id,
			      u8 length, const u8 *data);

/**
 * wilco_ec_get_bool_prop() - Get a boolean property from EC.
 * @dev: EC device to use
 * @property_id: Property ID
 * @result: Destination buffer to be filled, needs to be able to hold at least
 *	    two bytes. Will be filled with either "0\n" or "1\n" in ASCII
 *
 * Return: Number of bytes copied into result (AKA 2),
 *	   or negative error code on failure.
 */
ssize_t wilco_ec_get_bool_prop(struct device *dev, u32 property_id,
			       char *result);

/**
 * wilco_ec_set_bool_prop() - Set a boolean property on EC
 * @dev: EC device to use
 * @op: either OP_SET or OP_SYNC
 * @property_id: Property ID
 * @buf: Source buffer of ASCII string, parseable by kstrtobool()
 * @count: Number of bytes in input buffer
 *
 * Return: Number of bytes consumed from input buffer (AKA @count),
 *         or negative error code on failure.
 */
ssize_t wilco_ec_set_bool_prop(struct device *dev, enum get_set_sync_op op,
			       u32 property_id, const char *buf, size_t count);

/**
 * wilco_ec_bool_prop_show() - Get a boolean property from the EC
 * @kobj: Kobject representing the directory this attribute lives within
 * @attr: Attribute stored within relevant "struct property_attribute"
 * @buf: Destination buffer to be filled, needs to be able to hold at least
 *	 two bytes. Will be filled with either "0\n" or "1\n" in ASCII
 *
 * Return: Number of bytes placed into output buffer (AKA 2),
 *	   or negative error code on failure.
 */
ssize_t wilco_ec_bool_prop_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf);

/**
 * wilco_ec_bool_prop_store() - Store a boolean property on the EC
 * @kobj: Kobject representing the directory this attribute lives within
 * @attr: Attribute stored within relevant "struct property_attribute"
 * @buf: Source buffer of ASCII string, parseable by kstrtobool()
 * @count: Number of bytes in input buffer
 *
 * Return: Number bytes consumed from input buf (AKA @count),
 *	   or negative error code on failure.
 */
ssize_t wilco_ec_bool_prop_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count);

#define BOOL_PROP_KOBJ_ATTR_RW(_name)					\
__ATTR(_name, 0644, wilco_ec_bool_prop_show, wilco_ec_bool_prop_store)

#define BOOL_PROP_KOBJ_ATTR_WO(_name)					\
__ATTR(_name, 0200, NULL, wilco_ec_bool_prop_store)

#define BOOLEAN_PROPERTY_RW_ATTRIBUTE(_op, _var, _name, _pid)	\
struct property_attribute _var = {					\
	.kobj_attr = BOOL_PROP_KOBJ_ATTR_RW(_name),			\
	.pid = _pid,							\
	.op = _op,							\
}

#define BOOLEAN_PROPERTY_WO_ATTRIBUTE(_op, _var, _name, _pid)	\
struct property_attribute _var = {					\
	.kobj_attr = BOOL_PROP_KOBJ_ATTR_WO(_name),			\
	.pid = _pid,							\
	.op = _op,							\
}

#endif /* WILCO_EC_PROPERTIES_H */
