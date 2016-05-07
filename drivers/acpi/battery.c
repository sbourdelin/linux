/*
 *  battery.c - ACPI Battery Driver (Revision: 2.0)
 *
 *  Copyright (C) 2007 Alexey Starikovskiy <astarikovskiy@suse.de>
 *  Copyright (C) 2004-2007 Vladimir Lebedev <vladimir.p.lebedev@intel.com>
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or (at
 *  your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/async.h>
#include <linux/acpi.h>
#include <linux/dmi.h>
#include "battery.h"

#define PREFIX "ACPI: "
#define ACPI_BATTERY_DEVICE_NAME	"Battery"

#define _COMPONENT		ACPI_BATTERY_COMPONENT

ACPI_MODULE_NAME("battery");

MODULE_AUTHOR("Paul Diefenbaugh");
MODULE_AUTHOR("Alexey Starikovskiy <astarikovskiy@suse.de>");
MODULE_DESCRIPTION("ACPI Battery Driver");
MODULE_LICENSE("GPL");

static async_cookie_t async_cookie;

static const struct acpi_device_id battery_device_ids[] = {
	{"PNP0C0A", 0},
	{"", 0},
};

MODULE_DEVICE_TABLE(acpi, battery_device_ids);

static int __init
battery_bix_broken_package_quirk(const struct dmi_system_id *d)
{
	battery_bix_broken_package = 1;
	return 0;
}

static int __init
battery_notification_delay_quirk(const struct dmi_system_id *d)
{
	battery_notification_delay_ms = 1000;
	return 0;
}

static const struct dmi_system_id bat_dmi_table[] __initconst = {
	{
		.callback = battery_bix_broken_package_quirk,
		.ident = "NEC LZ750/LS",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "NEC"),
			DMI_MATCH(DMI_PRODUCT_NAME, "PC-LZ750LS"),
		},
	},
	{
		.callback = battery_notification_delay_quirk,
		.ident = "Acer Aspire V5-573G",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Acer"),
			DMI_MATCH(DMI_PRODUCT_NAME, "Aspire V5-573G"),
		},
	},
	{},
};

static SIMPLE_DEV_PM_OPS(acpi_battery_pm, NULL, acpi_battery_common_resume);

static struct acpi_driver acpi_battery_driver = {
	.name = "battery",
	.class = ACPI_BATTERY_CLASS,
	.ids = battery_device_ids,
	.flags = ACPI_DRIVER_ALL_NOTIFY_EVENTS,
	.ops = {
		.add = acpi_battery_common_add,
		.remove = acpi_battery_common_remove,
		.notify = acpi_battery_common_notify,
		},
	.drv.pm = &acpi_battery_pm,
};

static void __init acpi_battery_init_async(void *unused, async_cookie_t cookie)
{
	int result;

	dmi_check_system(bat_dmi_table);

#ifdef CONFIG_ACPI_PROCFS_POWER
	acpi_battery_dir = acpi_lock_battery_dir();
	if (!acpi_battery_dir)
		return;
#endif
	result = acpi_bus_register_driver(&acpi_battery_driver);
#ifdef CONFIG_ACPI_PROCFS_POWER
	if (result < 0)
		acpi_unlock_battery_dir(acpi_battery_dir);
#endif
}

static int __init acpi_battery_init(void)
{
	if (acpi_disabled)
		return -ENODEV;

	async_cookie = async_schedule(acpi_battery_init_async, NULL);
	return 0;
}

static void __exit acpi_battery_exit(void)
{
	async_synchronize_cookie(async_cookie);
	acpi_bus_unregister_driver(&acpi_battery_driver);
#ifdef CONFIG_ACPI_PROCFS_POWER
	acpi_unlock_battery_dir(acpi_battery_dir);
#endif
}

module_init(acpi_battery_init);
module_exit(acpi_battery_exit);
