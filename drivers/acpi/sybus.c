/*
 * ACPI System Bus Device (\_SB, LNXSYBUS) Driver
 * ACPI System Bus Device Driver is used to handle events reported to
 * the device.
 *
 * Copyright (c) 2016, The Linux Foundation. All rights reserved.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/acpi.h>
#include <linux/workqueue.h>
#include <linux/reboot.h>

#include <acpi/acpi_drivers.h>

#define _COMPONENT			ACPI_BUS_COMPONENT
#define SYBUS_PFX			"ACPI SYBUS: "

/*
 * According to section 6.3.5 of ACPI 6.0 spec, the kernel
 * should evaluate _OST (an ACPI control method) every 10 seconds
 * to indicate "OS shutdown in progress" to the platform.
 */
#define SYBUS_INDICATE_INTERVAL		10000

#define SYBUS_NOTIFY_RESERVED		    (0x80)
#define SYBUS_NOTIFY_SHUTDOWN_REQUEST	(0x81)

ACPI_MODULE_NAME("sybus");

static void sybus_evaluate_ost(struct work_struct *);

static struct acpi_device_id acpi_sybus_ids[] = {
	{ACPI_BUS_HID, 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, acpi_sybus_ids);

static acpi_handle sybus_handle;
static DECLARE_DELAYED_WORK(acpi_sybus_work, sybus_evaluate_ost);

static void sybus_evaluate_ost(struct work_struct *dummy)
{
	pr_info(SYBUS_PFX "OS shutdown in progress.\n");
	acpi_evaluate_ost(sybus_handle, ACPI_OST_EC_OSPM_SHUTDOWN,
			  ACPI_OST_SC_OS_SHUTDOWN_IN_PROGRESS, NULL);
	schedule_delayed_work(&acpi_sybus_work,
			msecs_to_jiffies(SYBUS_INDICATE_INTERVAL));
}

static void acpi_sybus_notify(struct acpi_device *device, u32 event)
{
	/*
	 * The only event that ACPI System Bus Device should handle is
	 * SYBUS_NOTIFY_SHUTDOWN_REQUEST.
	 */
	if (event != SYBUS_NOTIFY_SHUTDOWN_REQUEST) {
		ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			"event %x is not supported by ACPI system bus device.\n",
			event));
		return;
	}

	pr_warn(SYBUS_PFX "Shutdown request notification received.\n");

	if (!delayed_work_pending(&acpi_sybus_work)) {
		sybus_evaluate_ost(NULL);
		schedule_delayed_work(&acpi_sybus_work,
				msecs_to_jiffies(SYBUS_INDICATE_INTERVAL));

		orderly_poweroff(true);
	} else
		pr_info(SYBUS_PFX "Shutdown in already progress!\n");
}

static int acpi_sybus_add(struct acpi_device *device)
{
	/* Only one ACPI system bus device */
	if (sybus_handle)
		return -EINVAL;
	sybus_handle = device->handle;
	return 0;
}

static int acpi_sybus_remove(struct acpi_device *device)
{
	cancel_delayed_work_sync(&acpi_sybus_work);
	sybus_handle = NULL;
	return 0;
}

static struct acpi_driver acpi_sybus_driver = {
	.name = "system_bus_device",
	.class = "system_bus",
	.ids = acpi_sybus_ids,
	.flags = ACPI_DRIVER_ALL_NOTIFY_EVENTS,
	.ops = {
		.notify = acpi_sybus_notify,
		.add = acpi_sybus_add,
		.remove = acpi_sybus_remove,
	},
};
module_acpi_driver(acpi_sybus_driver);

MODULE_DESCRIPTION("ACPI System Bus Device Driver");
MODULE_LICENSE("GPL");
