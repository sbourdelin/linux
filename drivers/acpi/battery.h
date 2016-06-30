/*
 * battery.h
 * Copyright (c) 2016, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#ifndef __ACPI_BATTERY_H
#define __ACPI_BATTERY_H

#define ACPI_BATTERY_CLASS "battery"

#define ACPI_BATTERY_NOTIFY_STATUS	0x80
#define ACPI_BATTERY_NOTIFY_INFO	0x81
#define ACPI_BATTERY_NOTIFY_THRESHOLD   0x82

extern int battery_bix_broken_package;
extern int battery_notification_delay_ms;
extern struct proc_dir_entry *acpi_battery_dir;

int acpi_battery_common_add(struct acpi_device *device);
int acpi_battery_common_remove(struct acpi_device *device);
int acpi_battery_common_resume(struct device *dev);
void acpi_battery_common_notify(struct acpi_device *device, u32 event);

/* Defined in cm_sbs.c */
#ifdef CONFIG_ACPI_PROCFS_POWER
struct proc_dir_entry *acpi_lock_battery_dir(void);
void *acpi_unlock_battery_dir(struct proc_dir_entry *acpi_battery_dir);
#endif

#endif
