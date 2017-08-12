/*
 * Enforce MSI driver loaded by PCIe controller driver
 *
 * Copyright (c) 2017, MACOM Technology Solutions Corporation
 * Author: Khuong Dinh <kdinh@apm.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/acpi.h>
#include "internal.h"

static acpi_status acpi_create_msi_device(acpi_handle handle, u32 Level,
					void *context, void **retval)
{
	struct acpi_device *device = NULL;
	int type = ACPI_BUS_TYPE_DEVICE;
	unsigned long long sta;
	acpi_status status;
	int ret = 0;

	acpi_bus_get_device(handle, &device);
	status = acpi_bus_get_status_handle(handle, &sta);
	if (ACPI_FAILURE(status))
		sta = 0;

	device = kzalloc(sizeof(struct acpi_device), GFP_KERNEL);
	if (!device)
		return -ENOMEM;

	acpi_init_device_object(device, handle, type, sta);
	ret = acpi_device_add(device, NULL);
	if (ret)
		return ret;
	device->parent = kzalloc(sizeof(struct acpi_device), GFP_KERNEL);
	INIT_LIST_HEAD(&device->parent->physical_node_list);

	acpi_device_add_finalize(device);

	ret = device_attach(&device->dev);
	if (ret < 0)
		return ret;

	acpi_create_platform_device(device, NULL);
	acpi_device_set_enumerated(device);

	return ret;
}

static const struct acpi_device_id acpi_msi_device_ids[] = {
	{"APMC0D0E", 0},
	{ }
};

int __init acpi_msi_init(void)
{
	acpi_status status;
	int ret = 0;

	status = acpi_get_devices(acpi_msi_device_ids[0].id,
			acpi_create_msi_device, NULL, NULL);
	if (ACPI_FAILURE(status))
		ret = -ENODEV;

	return ret;
}
