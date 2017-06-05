/*
 * Heterogeneous memory initiator sysfs attributes
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

static ssize_t firmware_id_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct memory_initiator *init = to_memory_initiator(dev);

	return sprintf(buf, "%d\n", init->pxm);
}
static DEVICE_ATTR_RO(firmware_id);

static ssize_t is_enabled_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct memory_initiator *init = to_memory_initiator(dev);
	int is_enabled;

	if (init->cpu)
		is_enabled = !!(init->cpu->flags & ACPI_SRAT_CPU_ENABLED);
	else if (init->x2apic)
		is_enabled = !!(init->x2apic->flags & ACPI_SRAT_CPU_ENABLED);
	else
		is_enabled = !!(init->gicc->flags & ACPI_SRAT_GICC_ENABLED);

	return sprintf(buf, "%d\n", is_enabled);
}
static DEVICE_ATTR_RO(is_enabled);

static struct attribute *memory_initiator_attributes[] = {
	&dev_attr_firmware_id.attr,
	&dev_attr_is_enabled.attr,
	NULL,
};

static struct attribute_group memory_initiator_attribute_group = {
	.attrs = memory_initiator_attributes,
};

const struct attribute_group *memory_initiator_attribute_groups[] = {
	&memory_initiator_attribute_group,
	NULL,
};
