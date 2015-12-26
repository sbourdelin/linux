/*
 * Asus Wireless Radio Control Driver
 *
 * Copyright (C) 2015 Endless Mobile, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/pci_ids.h>

#define ASUS_WIRELESS_MODULE_NAME "Asus Wireless Radio Control Driver"

struct asus_wireless_data {
	struct input_dev *inputdev;
};

static void asus_wireless_notify(struct acpi_device *device, u32 event)
{
	struct asus_wireless_data *data = acpi_driver_data(device);

	pr_debug("event=0x%X\n", event);
	if (event != 0x88) {
		pr_info("Unknown ASHS event: 0x%X\n", event);
		return;
	}
	input_report_key(data->inputdev, KEY_RFKILL, 1);
	input_report_key(data->inputdev, KEY_RFKILL, 0);
	input_sync(data->inputdev);
}

static int asus_wireless_add(struct acpi_device *device)
{
	struct asus_wireless_data *data;
	int err = -ENOMEM;

	pr_info(ASUS_WIRELESS_MODULE_NAME"\n");
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	device->driver_data = data;

	data->inputdev = input_allocate_device();
	if (!data->inputdev)
		goto fail;

	data->inputdev->name = "Asus Wireless Radio Control";
	data->inputdev->phys = "asus-wireless/input0";
	data->inputdev->id.bustype = BUS_HOST;
	data->inputdev->id.vendor = PCI_VENDOR_ID_ASUSTEK;
	data->inputdev->dev.parent = &device->dev;
	set_bit(EV_REP, data->inputdev->evbit);
	set_bit(KEY_RFKILL, data->inputdev->keybit);

	err = input_register_device(data->inputdev);
	if (err)
		goto fail;
	return 0;

fail:
	device->driver->ops.remove(device);
	return err;
}

static int asus_wireless_remove(struct acpi_device *device)
{
	struct asus_wireless_data *data = acpi_driver_data(device);

	pr_info("Removing "ASUS_WIRELESS_MODULE_NAME"\n");
	if (data->inputdev)
		input_unregister_device(data->inputdev);
	kfree(data);
	return 0;
}

static const struct acpi_device_id device_ids[] = {
	{"ATK4001", 0},
	{"ATK4002", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, device_ids);

static struct acpi_driver asus_wireless_driver = {
	.name = ASUS_WIRELESS_MODULE_NAME,
	.class = "hotkey",
	.ids = device_ids,
	.ops = {
		.add = asus_wireless_add,
		.remove = asus_wireless_remove,
		.notify = asus_wireless_notify,
	},
};
module_acpi_driver(asus_wireless_driver);

MODULE_DESCRIPTION(ASUS_WIRELESS_MODULE_NAME);
MODULE_AUTHOR("João Paulo Rechi Vita <jprvita@gmail.com>");
MODULE_LICENSE("GPL");
