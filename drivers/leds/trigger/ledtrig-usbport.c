/*
 * USB port LED trigger
 *
 * Copyright (C) 2016 Rafał Miłecki <rafal.milecki@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/device.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include "../leds.h"

struct usbport_trig_port {
	char *name;
	struct list_head list;
};

struct usbport_trig_data {
	struct led_classdev *led_cdev;
	struct list_head ports;
	struct notifier_block nb;
	int count; /* Amount of connected matching devices */
};

static ssize_t ports_show(struct device *dev, struct device_attribute *attr,
			  char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct usbport_trig_data *usbport_data = led_cdev->trigger_data;
	struct usbport_trig_port *port;
	ssize_t ret = 0;
	int len;

	list_for_each_entry(port, &usbport_data->ports, list) {
		len = sprintf(buf + ret, "%s\n", port->name);
		if (len >= 0)
			ret += len;
	}

	return ret;
}

static DEVICE_ATTR(ports, S_IRUSR, ports_show, NULL);

static ssize_t new_port_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct usbport_trig_data *usbport_data = led_cdev->trigger_data;
	struct usbport_trig_port *port;
	size_t len;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	len = strlen(buf);
	if (len && buf[len - 1] == '\n')
		len--;
	if (!len)
		return -EINVAL;

	port->name = kzalloc(strlen(buf), GFP_KERNEL);
	if (!port->name) {
		kfree(port);
		return -ENOMEM;
	}
	strncpy(port->name, buf, len);

	list_add_tail(&port->list, &usbport_data->ports);

	return size;
}

static DEVICE_ATTR(new_port, S_IWUSR, NULL, new_port_store);

static bool usbport_trig_match(struct usbport_trig_data *usbport_data,
			       struct usb_device *usb_dev)
{
	struct usbport_trig_port *port;
	const char *name = dev_name(&usb_dev->dev);

	list_for_each_entry(port, &usbport_data->ports, list) {
		if (!strcmp(port->name, name))
			return true;
	}

	return false;
}

static int usbport_trig_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct usbport_trig_data *usbport_data =
		container_of(nb, struct usbport_trig_data, nb);
	struct led_classdev *led_cdev = usbport_data->led_cdev;

	switch (action) {
	case USB_DEVICE_ADD:
		if (usbport_trig_match(usbport_data, data)) {
			if (usbport_data->count++ == 0)
				led_set_brightness_nosleep(led_cdev, LED_FULL);
		}
		break;
	case USB_DEVICE_REMOVE:
		if (usbport_trig_match(usbport_data, data)) {
			if (--usbport_data->count == 0)
				led_set_brightness_nosleep(led_cdev, LED_OFF);
		}
		break;
	}

	return NOTIFY_OK;
}

static void usbport_trig_activate(struct led_classdev *led_cdev)
{
	struct usbport_trig_data *usbport_data;
	int err;

	usbport_data = kzalloc(sizeof(*usbport_data), GFP_KERNEL);
	if (!usbport_data)
		return;
	usbport_data->led_cdev = led_cdev;
	INIT_LIST_HEAD(&usbport_data->ports);
	usbport_data->nb.notifier_call = usbport_trig_notify,
	led_cdev->trigger_data = usbport_data;

	err = device_create_file(led_cdev->dev, &dev_attr_ports);
	if (err)
		goto err_free;
	err = device_create_file(led_cdev->dev, &dev_attr_new_port);
	if (err)
		goto err_remove_ports;

	usb_register_notify(&usbport_data->nb);

	led_cdev->activated = true;
	return;

err_free:
	kfree(usbport_data);
err_remove_ports:
	device_remove_file(led_cdev->dev, &dev_attr_ports);
}

static void usbport_trig_deactivate(struct led_classdev *led_cdev)
{
	struct usbport_trig_data *usbport_data = led_cdev->trigger_data;
	struct usbport_trig_port *port, *tmp;

	if (!led_cdev->activated)
		return;

	usb_unregister_notify(&usbport_data->nb);

	list_for_each_entry_safe(port, tmp, &usbport_data->ports, list) {
		kfree(port->name);
		list_del(&port->list);
	}

	device_remove_file(led_cdev->dev, &dev_attr_new_port);
	device_remove_file(led_cdev->dev, &dev_attr_ports);
	kfree(usbport_data);

	led_cdev->activated = false;
}

static struct led_trigger usbport_led_trigger = {
	.name     = "usbport",
	.activate = usbport_trig_activate,
	.deactivate = usbport_trig_deactivate,
};

static int __init usbport_trig_init(void)
{
	return led_trigger_register(&usbport_led_trigger);
}

static void __exit usbport_trig_exit(void)
{
	led_trigger_unregister(&usbport_led_trigger);
}

module_init(usbport_trig_init);
module_exit(usbport_trig_exit);

MODULE_AUTHOR("Rafał Miłecki <rafal.milecki@gmail.com>");
MODULE_DESCRIPTION("USB port trigger");
MODULE_LICENSE("GPL");
