/*
 * Asus Wireless Radio Control Driver
 *
 * Copyright (C) 2015-2016 Endless Mobile, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/acpi.h>
#include <linux/input.h>
#include <linux/pci_ids.h>

struct asus_wireless_data {
	struct input_dev *inputdev;
};

static void asus_wireless_notify(struct acpi_device *device, u32 event)
{
	struct asus_wireless_data *data = acpi_driver_data(device);

	dev_dbg(&device->dev, "event=0x%X\n", event);
	if (event != 0x88) {
		dev_notice(&device->dev, "Unknown ASHS event: 0x%X\n", event);
		return;
	}
	input_report_key(data->inputdev, KEY_RFKILL, 1);
	input_report_key(data->inputdev, KEY_RFKILL, 0);
	input_sync(data->inputdev);
}

static int asus_wireless_add(struct acpi_device *device)
{
	struct asus_wireless_data *data;

	data = devm_kzalloc(&device->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	device->driver_data = data;

	data->inputdev = devm_input_allocate_device(&device->dev);
	if (!data->inputdev)
		return -ENOMEM;
	data->inputdev->name = "Asus Wireless Radio Control";
	data->inputdev->phys = "asus-wireless/input0";
	data->inputdev->id.bustype = BUS_HOST;
	data->inputdev->id.vendor = PCI_VENDOR_ID_ASUSTEK;
	set_bit(EV_KEY, data->inputdev->evbit);
	set_bit(KEY_RFKILL, data->inputdev->keybit);
	return input_register_device(data->inputdev);
}

static int asus_wireless_remove(struct acpi_device *device)
{
	return 0;
}

static const struct acpi_device_id device_ids[] = {
	{"ATK4002", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, device_ids);

static struct acpi_driver asus_wireless_driver = {
	.name = "Asus Wireless Radio Control Driver",
	.class = "hotkey",
	.ids = device_ids,
	.ops = {
		.add = asus_wireless_add,
		.remove = asus_wireless_remove,
		.notify = asus_wireless_notify,
	},
};
module_acpi_driver(asus_wireless_driver);

MODULE_DESCRIPTION("Asus Wireless Radio Control Driver");
MODULE_AUTHOR("João Paulo Rechi Vita <jprvita@gmail.com>");
MODULE_LICENSE("GPL");
