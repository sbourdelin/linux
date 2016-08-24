/*
 * USB port LED trigger
 *
 * Copyright (C) 2016 Rafał Miłecki <rafal@milecki.pl>
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
	struct device_attribute attr;
	struct list_head list;
};

struct usbport_trig_data {
	struct led_classdev *led_cdev;
	struct list_head ports;
	struct kobject *ports_dir;
	struct notifier_block nb;
	int count; /* Amount of connected matching devices */
};

/*
 * Helpers
 */

/**
 * usbport_trig_usb_dev_observed - Check if dev is connected to observerd port
 */
static bool usbport_trig_usb_dev_observed(struct usbport_trig_data *usbport_data,
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

static int usbport_trig_usb_dev_check(struct usb_device *usb_dev, void *data)
{
	struct usbport_trig_data *usbport_data = data;

	if (usbport_trig_usb_dev_observed(usbport_data, usb_dev))
		usbport_data->count++;

	return 0;
}

/**
 * usbport_trig_update_count - Recalculate amount of connected matching devices
 */
static void usbport_trig_update_count(struct usbport_trig_data *usbport_data)
{
	struct led_classdev *led_cdev = usbport_data->led_cdev;

	usbport_data->count = 0;
	usb_for_each_dev(usbport_data, usbport_trig_usb_dev_check);
	led_set_brightness_nosleep(led_cdev,
				   usbport_data->count ? LED_FULL : LED_OFF);
}

static int usbport_trig_add_port(struct usbport_trig_data *usbport_data,
				 const char *name)
{
	struct usbport_trig_port *port;
	int err;

	port = kzalloc(sizeof(*port), GFP_KERNEL);
	if (!port) {
		err = -ENOMEM;
		goto err_out;
	}

	port->name = kzalloc(strlen(name) + 1, GFP_KERNEL);
	if (!port->name) {
		err = -ENOMEM;
		goto err_free_port;
	}
	strcpy(port->name, name);

	port->attr.attr.name = port->name;

	err = sysfs_create_file(usbport_data->ports_dir, &port->attr.attr);
	if (err)
		goto err_free_name;

	list_add_tail(&port->list, &usbport_data->ports);

	return 0;

err_free_name:
	kfree(port->name);
err_free_port:
	kfree(port);
err_out:
	return err;
}

static void usbport_trig_remove_port(struct usbport_trig_data *usbport_data,
				     struct usbport_trig_port *port)
{
	list_del(&port->list);
	sysfs_remove_file(usbport_data->ports_dir, &port->attr.attr);
	kfree(port->name);
	kfree(port);
}

/*
 * Device attr
 */

static ssize_t new_port_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct usbport_trig_data *usbport_data = led_cdev->trigger_data;
	struct usbport_trig_port *port;
	char *name;
	size_t len;

	len = strlen(buf);
	/* For user convenience trim line break */
	if (len && buf[len - 1] == '\n')
		len--;
	if (!len)
		return -EINVAL;

	name = kzalloc(len + 1, GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	strncpy(name, buf, len);

	list_for_each_entry(port, &usbport_data->ports, list) {
		if (!strcmp(port->name, name))
			return -EEXIST;
	}

	usbport_trig_add_port(usbport_data, name);

	kfree(name);

	usbport_trig_update_count(usbport_data);

	return size;
}

static DEVICE_ATTR(new_port, S_IWUSR, NULL, new_port_store);

static ssize_t remove_port_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t size)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct usbport_trig_data *usbport_data = led_cdev->trigger_data;
	struct usbport_trig_port *port, *tmp;
	size_t len;

	len = strlen(buf);
	/* For user convenience trim line break */
	if (len && buf[len - 1] == '\n')
		len--;
	if (!len)
		return -EINVAL;

	list_for_each_entry_safe(port, tmp, &usbport_data->ports,
					list) {
		if (strlen(port->name) == len &&
		    !strncmp(port->name, buf, len)) {
			usbport_trig_remove_port(usbport_data, port);
			usbport_trig_update_count(usbport_data);
			return size;
		}
	}

	return -ENOENT;
}

static DEVICE_ATTR(remove_port, S_IWUSR, NULL, remove_port_store);

/*
 * Init, exit, etc.
 */

static int usbport_trig_notify(struct notifier_block *nb, unsigned long action,
			       void *data)
{
	struct usbport_trig_data *usbport_data =
		container_of(nb, struct usbport_trig_data, nb);
	struct led_classdev *led_cdev = usbport_data->led_cdev;

	if (!usbport_trig_usb_dev_observed(usbport_data, data))
		return NOTIFY_DONE;

	switch (action) {
	case USB_DEVICE_ADD:
		if (usbport_data->count++ == 0)
			led_set_brightness_nosleep(led_cdev, LED_FULL);
		return NOTIFY_OK;
	case USB_DEVICE_REMOVE:
		if (--usbport_data->count == 0)
			led_set_brightness_nosleep(led_cdev, LED_OFF);
		return NOTIFY_OK;
	}

	return NOTIFY_DONE;
}

static void usbport_trig_activate(struct led_classdev *led_cdev)
{
	struct usbport_trig_data *usbport_data;
	int err;

	usbport_data = kzalloc(sizeof(*usbport_data), GFP_KERNEL);
	if (!usbport_data)
		return;
	usbport_data->led_cdev = led_cdev;

	/* Storing ports */
	INIT_LIST_HEAD(&usbport_data->ports);
	usbport_data->ports_dir = kobject_create_and_add("ports",
							 &led_cdev->dev->kobj);
	if (!usbport_data->ports_dir)
		goto err_free;

	/* API for ports management */
	err = device_create_file(led_cdev->dev, &dev_attr_new_port);
	if (err)
		goto err_put_ports;
	err = device_create_file(led_cdev->dev, &dev_attr_remove_port);
	if (err)
		goto err_remove_new_port;

	/* Notifications */
	usbport_data->nb.notifier_call = usbport_trig_notify,
	led_cdev->trigger_data = usbport_data;
	usb_register_notify(&usbport_data->nb);

	led_cdev->activated = true;
	return;

err_remove_new_port:
	device_remove_file(led_cdev->dev, &dev_attr_new_port);
err_put_ports:
	kobject_put(usbport_data->ports_dir);
err_free:
	kfree(usbport_data);
}

static void usbport_trig_deactivate(struct led_classdev *led_cdev)
{
	struct usbport_trig_data *usbport_data = led_cdev->trigger_data;
	struct usbport_trig_port *port, *tmp;

	if (!led_cdev->activated)
		return;

	list_for_each_entry_safe(port, tmp, &usbport_data->ports, list) {
		usbport_trig_remove_port(usbport_data, port);
	}

	usb_unregister_notify(&usbport_data->nb);

	device_remove_file(led_cdev->dev, &dev_attr_remove_port);
	device_remove_file(led_cdev->dev, &dev_attr_new_port);

	kobject_put(usbport_data->ports_dir);

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

MODULE_AUTHOR("Rafał Miłecki <rafal@milecki.pl>");
MODULE_DESCRIPTION("USB port trigger");
MODULE_LICENSE("GPL");
