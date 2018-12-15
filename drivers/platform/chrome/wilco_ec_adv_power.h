/* SPDX-License-Identifier: GPL-2.0 */
/*
 * wilco_ec_adv_power - peakshift and adv_batt_charging config of Wilco EC
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

/**
 * Peakshift:
 * For each weekday a start and end time to run in Peak Shift mode can be set.
 * During these times the system will run from the battery even if the AC is
 * attached as long as the battery stays above the threshold specified.
 * After the end time specified the system will run from AC if attached but
 * will not charge the battery. The system will again function normally using AC
 * and recharging the battery after the specified Charge Start time.
 *
 * Advanced Charging Mode:
 * Advanced Charging Mode allows the user to maximize the battery health.
 * In Advanced Charging Mode the system will use standard charging algorithm and
 * other techniques during non-work hours to maximize battery health.
 * During work hours, an express charge is used. This express charge allows the
 * battery to be charged faster; therefore, the battery is at
 * full charge sooner. For each day the time in which the system will be most
 * heavily used is specified by the start time and the duration.
 * Please read the Common UEFI BIOS Behavioral Specification and
 * BatMan 2 BIOS_EC Specification for more details about this feature.
 */

#ifndef WILCO_EC_ADV_POWER_H
#define WILCO_EC_ADV_POWER_H

#include <linux/kobject.h>
#include <linux/sysfs.h>
#include "wilco_ec.h"
#include "wilco_ec_sysfs_util.h"
#include "wilco_ec_properties.h"

#define PID_PEAKSHIFT				0x0412
#define PID_PEAKSHIFT_BATTERY_THRESHOLD		0x04EB
#define PID_PEAKSHIFT_SUNDAY_HOURS		0x04F5
#define PID_PEAKSHIFT_MONDAY_HOURS		0x04F6
#define PID_PEAKSHIFT_TUESDAY_HOURS		0x04F7
#define PID_PEAKSHIFT_WEDNESDAY_HOURS		0x04F8
#define PID_PEAKSHIFT_THURSDAY_HOURS		0x04F9
#define PID_PEAKSHIFT_FRIDAY_HOURS		0x04Fa
#define PID_PEAKSHIFT_SATURDAY_HOURS		0x04Fb

#define PID_ABC_MODE				0x04ed
#define PID_ABC_SUNDAY_HOURS			0x04F5
#define PID_ABC_MONDAY_HOURS			0x04F6
#define PID_ABC_TUESDAY_HOURS			0x04F7
#define PID_ABC_WEDNESDAY_HOURS			0x04F8
#define PID_ABC_THURSDAY_HOURS			0x04F9
#define PID_ABC_FRIDAY_HOURS			0x04FA
#define PID_ABC_SATURDAY_HOURS			0x04FB

/**
 * wilco_ec_peakshift_show() - Retrieves times stored for the peakshift policy.
 * @kobj: kobject representing the directory this attribute is in
 * @attr: Attribute stored within the proper property_attribute
 * @buf: Output buffer to fill with the result
 *
 * The output buffer will be filled with the format
 * "start_hr start_min end_hr end_min charge_start_hr charge_start_min"
 * The hour fields will be in the range [0-23], and the minutes will be
 * one of (0, 15, 30, 45). Each number will be zero padded to two characters.
 *
 * An example output is "06 15 09 45 23 00",
 * which corresponds to 6:15, 9:45, and 23:00
 *
 * Return the length of the output buffer, or negative error code on failure.
 */
ssize_t wilco_ec_peakshift_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf);

/**
 * wilco_ec_peakshift_store() - Saves times for the peakshift policy.
 * @kobj: kobject representing the directory this attribute is in
 * @attr: Attribute stored within the proper property_attribute
 * @buf: Raw input buffer
 * @count: Number of bytes in input buffer
 *
 * The input buffer must have the format
 * "start_hr start_min end_hr end_min charge_start_hr charge_start_min"
 * The hour fields must be in the range [0-23], and the minutes must be
 * one of (0, 15, 30, 45). The string must be parseable by sscanf() using the
 * format string "%d %d %d %d %d %d".
 *
 * An example valid input is "6 15     009 45 23 0",
 * which corresponds to 6:15, 9:45, and 23:00
 *
 * Return number of bytes consumed from input, negative error code on failure.
 */
ssize_t wilco_ec_peakshift_store(struct kobject *kobj,
				 struct kobj_attribute *attr, const char *buf,
				 size_t count);

/**
 * peakshift_battery_show() - Retrieve batt percentage at which peakshift stops
 * @kobj: kobject representing the directory this attribute is in
 * @attr: Attribute stored within the proper property_attribute
 * @buf: Output buffer to fill with the result
 *
 * Result will be a 2 character integer representing the
 * battery percentage at which peakshift stops. Will be in range [15, 50].
 *
 * Return the length of the output buffer, or negative error code on failure.
 */
ssize_t wilco_ec_peakshift_batt_thresh_show(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    char *buf);

/**
 * peakshift_battery_store() - Save batt percentage at which peakshift stops
 * @kobj: kobject representing the directory this attribute is in
 * @attr: Attribute stored within the proper property_attribute
 * @buf: Input buffer, should be parseable to range [15,50] by kstrtou8()
 * @count: Number of bytes in input buffer
 *
 * Return number of bytes consumed from input, negative error code on failure.
 */
ssize_t wilco_ec_peakshift_batt_thresh_store(struct kobject *kobj,
					     struct kobj_attribute *attr,
					     const char *buf, size_t count);

/**
 * wilco_ec_abc_show() - Retrieve times for Advanced Battery Charging
 * @kobj: kobject representing the directory this attribute is in
 * @attr: Attribute stored within the proper property_attribute
 * @buf: Output buffer to fill with the result
 *
 * The output buffer will be filled with the format
 * "start_hr start_min duration_hr duration_min"
 * The hour fields will be in the range [0-23], and the minutes will be
 * one of (0, 15, 30, 45). Each number will be zero padded to two characters.
 *
 * An example output is "06 15 23 45",
 * which corresponds to a start time of 6:15 and a duration of 23:45
 *
 * Return the length of the output buffer, or negative error code on failure.
 */
ssize_t wilco_ec_abc_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf);

/**
 * wilco_ec_abc_store() - Save times for Advanced Battery Charging
 * @kobj: kobject representing the directory this attribute is in
 * @attr: Attribute stored within the proper property_attribute
 * @buf: The raw input buffer
 * @count: Number of bytes in input buffer
 *
 * The input buffer must have the format
 * "start_hr start_min duration_hr duration_min"
 * The hour fields must be in the range [0-23], and the minutes must be
 * one of (0, 15, 30, 45). The string must be parseable by sscanf() using the
 * format string "%d %d %d %d %d %d".
 *
 * An example valid input is "0006 15     23 45",
 * which corresponds to a start time of 6:15 and a duration of 23:45
 *
 * Return number of bytes consumed, or negative error code on failure.
 */
ssize_t wilco_ec_abc_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count);

#define PEAKSHIFT_KOBJ_ATTR(_name)					\
__ATTR(_name, 0644, wilco_ec_peakshift_show, wilco_ec_peakshift_store)

#define PEAKSHIFT_ATTRIBUTE(_var, _name, _pid)				\
struct property_attribute _var = {					\
	.kobj_attr = PEAKSHIFT_KOBJ_ATTR(_name),			\
	.pid = _pid,							\
}

#define ABC_KOBJ_ATTR(_name)						\
__ATTR(_name, 0644, wilco_ec_abc_show, wilco_ec_abc_store)

#define ABC_ATTRIBUTE(_var, _name, _pid)				\
struct property_attribute _var = {					\
	.kobj_attr = ABC_KOBJ_ATTR(_name),				\
	.pid = _pid,							\
}

#endif /* WILCO_EC_ADV_POWER_H */
