/*
 * Heterogeneous memory target sysfs attributes
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

#include <acpi/acpi_numa.h>
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/sysfs.h>
#include "hmem.h"

/* attributes for memory targets */
static ssize_t phys_addr_base_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct memory_target *tgt = to_memory_target(dev);

	return sprintf(buf, "%#llx\n", tgt->ma->base_address);
}
static DEVICE_ATTR_RO(phys_addr_base);

static ssize_t phys_length_bytes_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct memory_target *tgt = to_memory_target(dev);

	return sprintf(buf, "%#llx\n", tgt->ma->length);
}
static DEVICE_ATTR_RO(phys_length_bytes);

static ssize_t firmware_id_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct memory_target *tgt = to_memory_target(dev);

	return sprintf(buf, "%d\n", tgt->ma->proximity_domain);
}
static DEVICE_ATTR_RO(firmware_id);

static ssize_t is_cached_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct memory_target *tgt = to_memory_target(dev);

	return sprintf(buf, "%d\n", tgt->is_cached);
}
static DEVICE_ATTR_RO(is_cached);

static ssize_t is_isolated_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct memory_target *tgt = to_memory_target(dev);

	return sprintf(buf, "%d\n",
			!!(tgt->spa->flags & ACPI_HMAT_RESERVATION_HINT));
}
static DEVICE_ATTR_RO(is_isolated);

static ssize_t is_enabled_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct memory_target *tgt = to_memory_target(dev);

	return sprintf(buf, "%d\n",
			!!(tgt->ma->flags & ACPI_SRAT_MEM_ENABLED));
}
static DEVICE_ATTR_RO(is_enabled);

static struct attribute *memory_target_attributes[] = {
	&dev_attr_phys_addr_base.attr,
	&dev_attr_phys_length_bytes.attr,
	&dev_attr_firmware_id.attr,
	&dev_attr_is_cached.attr,
	&dev_attr_is_isolated.attr,
	&dev_attr_is_enabled.attr,
	NULL
};

/* attributes which are present for all memory targets */
static struct attribute_group memory_target_attribute_group = {
	.attrs = memory_target_attributes,
};

const struct attribute_group *memory_target_attribute_groups[] = {
	&memory_target_attribute_group,
	NULL,
};
