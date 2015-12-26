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
#include <linux/leds.h>

#define ASUS_WIRELESS_MODULE_NAME "Asus Wireless Radio Control Driver"
#define ASUS_WIRELESS_LED_STATUS 0x2
#define ASUS_WIRELESS_LED_OFF 0x4
#define ASUS_WIRELESS_LED_ON 0x5

struct asus_wireless_data {
	struct input_dev *inputdev;
	struct acpi_device *acpidev;
	struct workqueue_struct *wq;
	struct work_struct led_work;
	struct led_classdev led;
	int led_state;
};

static u64 asus_wireless_method(acpi_handle handle, const char *method,
				int param)
{
	union acpi_object obj;
	struct acpi_object_list p;
	acpi_status s;
	u64 ret;

	pr_debug("Evaluating method %s, parameter 0x%X\n", method, param);
	obj.type = ACPI_TYPE_INTEGER;
	obj.integer.value = param;
	p.count = 1;
	p.pointer = &obj;

	s = acpi_evaluate_integer(handle, (acpi_string) method, &p, &ret);
	if (!ACPI_SUCCESS(s))
		pr_err("Failed to evaluate method %s, parameter 0x%X (%d)\n",
		       method, param, s);
	pr_debug("%s returned 0x%X\n", method, (uint) ret);
	return ret;
}

static enum led_brightness asus_wireless_led_get(struct led_classdev *led)
{
	struct asus_wireless_data *data;
	int s;

	data = container_of(led, struct asus_wireless_data, led);
	s = asus_wireless_method(data->acpidev->handle, "HSWC",
				 ASUS_WIRELESS_LED_STATUS);
	if (s == ASUS_WIRELESS_LED_ON)
		return LED_FULL;
	return LED_OFF;
}

static void asus_wireless_led_update(struct work_struct *work)
{
	struct asus_wireless_data *data;

	data = container_of(work, struct asus_wireless_data, led_work);
	asus_wireless_method(data->acpidev->handle, "HSWC", data->led_state);
}

static void asus_wireless_led_set(struct led_classdev *led,
				  enum led_brightness value)
{
	struct asus_wireless_data *data;

	data = container_of(led, struct asus_wireless_data, led);
	data->led_state = value == LED_OFF ? ASUS_WIRELESS_LED_OFF :
					     ASUS_WIRELESS_LED_ON;
	queue_work(data->wq, &data->led_work);
}

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

	data->acpidev = device;
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

	data->wq = create_singlethread_workqueue("asus_wireless_workqueue");
	if (!data->wq)
		goto fail;

	INIT_WORK(&data->led_work, asus_wireless_led_update);
	data->led.name = "asus-wireless::airplane_mode";
	data->led.brightness_set = asus_wireless_led_set;
	data->led.brightness_get = asus_wireless_led_get;
	data->led.flags = LED_CORE_SUSPENDRESUME;
	data->led.max_brightness = 1;
	data->led.default_trigger = "rfkill-airplane-mode";
	err = led_classdev_register(&device->dev, &data->led);
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
	if (data->wq)
		destroy_workqueue(data->wq);
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
