/*
 * Copyright (C) 2015 Pengutronix, Uwe Kleine-König <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation.
 */

#include <linux/device.h>

#define to_siox_device(_dev)	container_of((_dev), struct siox_device, dev)
struct siox_device {
	struct list_head node; /* node in smaster->devices */
	struct siox_master *smaster;
	struct device dev;

	char *type;
	size_t inbytes;
	size_t outbytes;

	u8 status;

	/* statistics */
	unsigned int watchdog_errors;
	unsigned int status_errors;

	struct kernfs_node *status_errors_kn;
	struct kernfs_node *watchdog_kn;
	struct kernfs_node *watchdog_errors_kn;
};

#define to_siox_driver(_drv)	container_of((_drv), struct siox_driver, driver)
struct siox_driver {
	int (*probe)(struct siox_device *);
	int (*remove)(struct siox_device *);
	void (*shutdown)(struct siox_device *);

	/*
	 * buf is big enough to hold sdev->inbytes - 1 bytes, the status byte
	 * is in the scope of the framework.
	 */
	int (*set_data)(struct siox_device *, u8 status, u8 buf[]);
	/*
	 * buf is big enough to hold sdev->outbytes - 1 bytes, the status byte
	 * is in the scope of the framework
	 */
	int (*get_data)(struct siox_device *, const u8 buf[]);

	struct device_driver driver;
};

int __siox_driver_register(struct siox_driver *sdriver, struct module *owner);
static inline int siox_driver_register(struct siox_driver *sdriver)
{
	return __siox_driver_register(sdriver, THIS_MODULE);
}

static inline void siox_driver_unregister(struct siox_driver *sdriver)
{
	return driver_unregister(&sdriver->driver);
}
