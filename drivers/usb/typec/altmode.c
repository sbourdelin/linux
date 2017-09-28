/**
 * USB Type-C Alternate Mode bus
 *
 * Copyright (C) 2017 Intel Corporation
 * Author: Heikki Krogerus <heikki.krogerus@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/usb/typec_altmode.h>

#include "altmode.h"

/* -------------------------------------------------------------------------- */
/* Common API */

/**
 * typec_altmode_notify - Communicate with the platform
 * @altmode: Handle to the alternate mode
 * @conf: Alternate mode specific configuration value
 * @data: Alternate mode specific data to be passed to the partner
 *
 * The primary purpose for this function is to allow the alternate mode drivers
 * to tell the platform which pin configuration has been negotiated with the
 * partner, but communication to the other direction is also possible, so low
 * level device drivers can also send notifications to the alternate mode
 * drivers. The actual communication will be specific to every alternate mode.
 */
int typec_altmode_notify(struct typec_altmode *altmode,
			 unsigned long conf, void *data)
{
	struct typec_altmode *partner;

	if (!altmode)
		return 0;

	if (!altmode->partner)
		return -ENODEV;

	partner = altmode->partner;

	/*
	 * This is where we will later pass the data to the remote-endpoints,
	 * but for now simply passing the data to the port.
	 *
	 * More information about the remote-endpoint concept:
	 *   Documentation/acpi/dsd/graph.txt
	 *   Documentation/devicetree/bindings/graph.txt
	 *
	 * Check drivers/base/property.c to see the API for the endpoint
	 * handling (the fwnode_graph* functions).
	 */

	if (partner->ops && partner->ops->notify)
		return partner->ops->notify(partner, conf, data);

	return 0;
}
EXPORT_SYMBOL_GPL(typec_altmode_notify);

/**
 * typec_altmode_send_vdm - Send Vendor Defined Messages to the partner
 * @altmode: Alternate mode handle
 * @header: VDM Header
 * @vdo: Array of Vendor Defined Data Objects
 * @count: Number of Data Objects
 *
 * The alternate mode drivers use this function for SVID specific communication
 * with the partner. The port drivers use it to deliver the Structured VDMs
 * received from the partners to the alternate mode drivers.
 */
int typec_altmode_send_vdm(struct typec_altmode *altmode,
			   u32 header, u32 *vdo, int count)
{
	struct typec_altmode *partner;

	if (!altmode)
		return 0;

	if (!altmode->partner)
		return -ENODEV;

	partner = altmode->partner;

	if (partner->ops && partner->ops->vdm)
		partner->ops->vdm(partner, header, vdo, count);

	return 0;
}
EXPORT_SYMBOL_GPL(typec_altmode_send_vdm);

void typec_altmode_set_drvdata(struct typec_altmode *altmode, void *data)
{
	dev_set_drvdata(&altmode->dev, data);
}
EXPORT_SYMBOL_GPL(typec_altmode_set_drvdata);

void *typec_altmode_get_drvdata(struct typec_altmode *altmode)
{
	return dev_get_drvdata(&altmode->dev);
}
EXPORT_SYMBOL_GPL(typec_altmode_get_drvdata);

/* -------------------------------------------------------------------------- */
/* API for the alternate mode drivers */

/**
 * typec_altmode_register_ops - Register alternate mode specific operations
 * @altmode: Handle to the alternate mode
 * @ops: Alternate mode specific operations vector
 *
 * Used by the alternate mode drivers for registering their operation vectors
 * with the alternate mode device.
 */
void typec_altmode_register_ops(struct typec_altmode *altmode,
				struct typec_altmode_ops *ops)
{
	altmode->ops = ops;
}
EXPORT_SYMBOL_GPL(typec_altmode_register_ops);

/**
 * typec_altmode_get_plug - Find cable plug alternate mode
 * @altmode: Handle to partner alternate mode
 * @index: Cable plug index
 *
 * Increment reference count for cable plug alternate mode device. Returns
 * handle to the cable plug alternate mode, or NULL if none is found.
 */
struct typec_altmode *typec_altmode_get_plug(struct typec_altmode *altmode,
					     int index)
{
	if (altmode->partner->plug[index]) {
		get_device(&altmode->partner->plug[index]->dev);
		return altmode->partner->plug[index];
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(typec_altmode_get_plug);

/**
 * typec_altmode_get_plug - Decrement cable plug alternate mode reference count
 * @plug: Handle to the cable plug alternate mode
 */
void typec_altmode_put_plug(struct typec_altmode *plug)
{
	if (plug)
		put_device(&plug->dev);
}
EXPORT_SYMBOL_GPL(typec_altmode_put_plug);

/* -------------------------------------------------------------------------- */
/* API for the port drivers */

/**
 * typec_find_altmode - Match SVID to an array of alternate modes
 * @altmodes: Array of alternate modes
 * @n: Number of elements in the array, or -1 for NULL termiated arrays
 * @svid: Standard or Vendor ID to match with
 *
 * Return pointer to an alternate mode with SVID mathing @svid, or NULL when no
 * match is found.
 */
struct typec_altmode *typec_find_altmode(struct typec_altmode **altmodes,
					 size_t n, u16 svid)
{
	int i;

	for (i = 0; i < n; i++) {
		if (!altmodes[i] || !altmodes[i]->svid)
			break;
		if (altmodes[i]->svid == svid)
			return altmodes[i];
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(typec_find_altmode);

/* -------------------------------------------------------------------------- */

static int typec_altmode_match(struct device *dev, struct device_driver *driver)
{
	struct typec_altmode_driver *drv = to_altmode_driver(driver);
	struct typec_altmode *altmode = to_altmode(dev);

	return drv->svid == altmode->svid;
}

static int typec_altmode_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	struct typec_altmode *altmode = to_altmode(dev);

	return add_uevent_var(env, "MODALIAS=svid:%04x", altmode->svid);
}

static int typec_altmode_probe(struct device *dev)
{
	struct typec_altmode_driver *drv = to_altmode_driver(dev->driver);
	struct typec_altmode *altmode = to_altmode(dev);

	/* Fail if the port does not support the alternate mode */
	if (!altmode->partner)
		return -ENODEV;

	return drv->probe(altmode);
}

static int typec_altmode_remove(struct device *dev)
{
	struct typec_altmode_driver *drv = to_altmode_driver(dev->driver);

	if (drv->remove)
		drv->remove(to_altmode(dev));

	return 0;
}

struct bus_type typec_altmode_bus = {
	.name = "typec_altmode",
	.match = typec_altmode_match,
	.uevent = typec_altmode_uevent,
	.probe = typec_altmode_probe,
	.remove = typec_altmode_remove,
};

/* -------------------------------------------------------------------------- */

int __typec_altmode_register_driver(struct typec_altmode_driver *drv,
				    struct module *module)
{
	if (!drv->probe)
		return -EINVAL;

	drv->driver.owner = module;
	drv->driver.bus = &typec_altmode_bus;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__typec_altmode_register_driver);

void typec_altmode_unregister_driver(struct typec_altmode_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(typec_altmode_unregister_driver);
