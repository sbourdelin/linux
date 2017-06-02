/*
 * Heterogeneous memory performance attributes
 *
 * Copyright (c) 2017, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include "hmem.h"

#define NO_VALUE	-1
#define LATENCY		0
#define BANDWIDTH	1

/* Performance attributes for an initiator/target pair. */
static int get_performance_data(u32 init_pxm, u32 tgt_pxm,
		struct acpi_hmat_locality *hmat_loc)
{
	int num_init = hmat_loc->number_of_initiator_Pds;
	int num_tgt = hmat_loc->number_of_target_Pds;
	int init_idx = NO_VALUE;
	int tgt_idx = NO_VALUE;
	u32 *initiators, *targets;
	u16 *entries, val;
	int i;

	initiators = hmat_loc->data;
	targets = &initiators[num_init];
	entries = (u16 *)&targets[num_tgt];

	for (i = 0; i < num_init; i++) {
		if (initiators[i] == init_pxm) {
			init_idx = i;
			break;
		}
	}

	if (init_idx == NO_VALUE)
		return NO_VALUE;

	for (i = 0; i < num_tgt; i++) {
		if (targets[i] == tgt_pxm) {
			tgt_idx = i;
			break;
		}
	}

	if (tgt_idx == NO_VALUE)
		return NO_VALUE;

	val = entries[init_idx*num_tgt + tgt_idx];
	if (val < 10 || val == 0xFFFF)
		return NO_VALUE;

	return (val * hmat_loc->entry_base_unit) / 10;
}

/*
 * 'direction' is either READ or WRITE
 * 'type' is either LATENCY or BANDWIDTH
 * Latency is reported in nanoseconds and bandwidth is reported in MB/s.
 */
static int get_dev_attribute(struct device *dev, int direction, int type)
{
	struct memory_target *tgt = to_memory_target(dev);
	int tgt_pxm = tgt->ma->proximity_domain;
	int init_pxm = tgt->local_init->pxm;
	struct memory_locality *loc;
	int value;

	list_for_each_entry(loc, &locality_list, list) {
		struct acpi_hmat_locality *hmat_loc = loc->hmat_loc;

		if (direction == READ && type == LATENCY &&
		    (hmat_loc->data_type == ACPI_HMAT_ACCESS_LATENCY ||
		     hmat_loc->data_type == ACPI_HMAT_READ_LATENCY)) {
			value = get_performance_data(init_pxm, tgt_pxm,
					hmat_loc);
			if (value != NO_VALUE)
				return value;
		}

		if (direction == WRITE && type == LATENCY &&
		    (hmat_loc->data_type == ACPI_HMAT_ACCESS_LATENCY ||
		     hmat_loc->data_type == ACPI_HMAT_WRITE_LATENCY)) {
			value = get_performance_data(init_pxm, tgt_pxm,
					hmat_loc);
			if (value != NO_VALUE)
				return value;
		}

		if (direction == READ && type == BANDWIDTH &&
		    (hmat_loc->data_type == ACPI_HMAT_ACCESS_BANDWIDTH ||
		     hmat_loc->data_type == ACPI_HMAT_READ_BANDWIDTH)) {
			value = get_performance_data(init_pxm, tgt_pxm,
					hmat_loc);
			if (value != NO_VALUE)
				return value;
		}

		if (direction == WRITE && type == BANDWIDTH &&
		    (hmat_loc->data_type == ACPI_HMAT_ACCESS_BANDWIDTH ||
		     hmat_loc->data_type == ACPI_HMAT_WRITE_BANDWIDTH)) {
			value = get_performance_data(init_pxm, tgt_pxm,
					hmat_loc);
			if (value != NO_VALUE)
				return value;
		}
	}

	return NO_VALUE;
}

static ssize_t read_lat_nsec_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", get_dev_attribute(dev, READ, LATENCY));
}
static DEVICE_ATTR_RO(read_lat_nsec);

static ssize_t write_lat_nsec_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", get_dev_attribute(dev, WRITE, LATENCY));
}
static DEVICE_ATTR_RO(write_lat_nsec);

static ssize_t read_bw_MBps_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", get_dev_attribute(dev, READ, BANDWIDTH));
}
static DEVICE_ATTR_RO(read_bw_MBps);

static ssize_t write_bw_MBps_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d\n", get_dev_attribute(dev, WRITE, BANDWIDTH));
}
static DEVICE_ATTR_RO(write_bw_MBps);

struct attribute *performance_attributes[] = {
	&dev_attr_read_lat_nsec.attr,
	&dev_attr_write_lat_nsec.attr,
	&dev_attr_read_bw_MBps.attr,
	&dev_attr_write_bw_MBps.attr,
	NULL
};
