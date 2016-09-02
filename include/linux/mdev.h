/*
 * Mediated device definition
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * Copyright (c) 2016 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef MDEV_H
#define MDEV_H

#include <uapi/linux/vfio.h>


/* mediated device */
struct mdev_device {
	struct device		dev;
	struct iommu_group	*group;
	uuid_le			uuid;
	void			*driver_data;
};

/**
 * struct mdev_host_ops - Structure to be registered for each host device to
 * to mdev.
 *
 * @owner:		The module owner.
 * @hdev_attr_groups:	Default attributes of the host device.
 * @mdev_attr_groups:	Default attributes of the mdev device.
 * @supported_config:	Called to get information about supported types.
 *			@dev : device structure of host device.
 *			@config: should return string listing supported config
 *			Returns integer: success (0) or error (< 0)
 * @create:		Called to allocate basic resources in host device's
 *			driver for a particular mediated device. It is
 *			mandatory to provide create ops.
 *			@mdev: mdev_device structure on of mediated device
 *			      that is being created
 *			@mdev_params: extra parameters required by host
 *			device's driver.
 *			Returns integer: success (0) or error (< 0)
 * @destroy:		Called to free resources in host device's driver for a
 *			a mediated device instance. It is mandatory to provide
 *			destroy ops.
 *			@mdev: mdev_device device structure which is being
 *				destroyed
 *			Returns integer: success (0) or error (< 0)
 *			If VMM is running and destroy() is called that means the
 *			mdev is being hotunpluged. Return error if VMM is
 *			running and driver doesn't support mediated device
 *			hotplug.
 * @start:		Called to initiate mediated device initialization
 *			process in host device's driver before VMM starts.
 *			@mdev: mediated device structure
 *			Returns integer: success (0) or error (< 0)
 * @stop:		Called to teardown mediated device related resources
 *			@mdev: mediated device structure
 *			Returns integer: success (0) or error (< 0)
 * @read:		Read emulation callback
 *			@mdev: mediated device structure
 *			@buf: read buffer
 *			@count: number of bytes to read
 *			@pos: address.
 *			Retuns number on bytes read on success or error.
 * @write:		Write emulation callback
 *			@mdev: mediated device structure
 *			@buf: write buffer
 *			@count: number of bytes to be written
 *			@pos: address.
 *			Retuns number on bytes written on success or error.
 * @mmap:		Memory Map
 *			@mdev: mediated device structure
 *			@pos: address
 *			@virtaddr: target user address to start at. Vendor
 *			driver can change if required.
 *			@pfn: host address of kernel memory, vendor driver
 *			can change if required.
 *			@size: size of map area, vendor driver can change the
 *			size of map area if desired.
 *			@prot: page protection flags for this mapping, vendor
 *			driver can change, if required.
 *			Returns integer: success (0) or error (< 0)
 *
 * Host device that support mediated device should be registered with mdev
 * module with mdev_host_ops structure.
 */
struct mdev_host_ops {
	struct module *owner;
	const struct attribute_group **hdev_attr_groups;
	const struct attribute_group **mdev_attr_groups;

	int (*supported_config)(struct device *dev, char *config);
	int (*create)(struct mdev_device *mdev, char *mdev_params);
	void (*destroy)(struct mdev_device *mdev);

	int (*start)(struct mdev_device *mdev);
	int (*stop)(struct mdev_device *mdev);

	ssize_t (*read)(struct mdev_device *mdev, char __user *buf,
			size_t count, loff_t *pos);
	ssize_t (*write)(struct mdev_device *mdev, const char __user *buf,
			size_t count, loff_t *pos);
	int (*mmap)(struct mdev_device *mdev, struct vm_area_struct *vma);
	long (*ioctl)(struct mdev_device *mdev, unsigned int cmd,
			unsigned long arg);
};

/* mdev host device */
struct mdev_host {
	struct device dev;
	const struct mdev_host_ops *ops;
};

/**
 * struct mdev_driver - Mediated device driver
 * @name: driver name
 * @probe: called when new device created
 * @remove: called when device removed
 * @driver: device driver structure
 **/
struct mdev_driver {
	const char *name;
	int (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	int (*online)(struct device *dev);
	int (*offline)(struct device *dev);
	struct device_driver driver;
};

static inline void *mdev_get_drvdata(struct mdev_device *mdev)
{
	return mdev->driver_data;
}

static inline void mdev_set_drvdata(struct mdev_device *mdev, void *data)
{
	mdev->driver_data = data;
}

extern struct bus_type mdev_bus_type;

#define to_mdev_driver(drv) container_of(drv, struct mdev_driver, driver)
#define dev_to_host(_dev) container_of((_dev), struct mdev_host, dev)
#define dev_to_mdev(_dev) container_of((_dev), struct mdev_device, dev)

struct mdev_host *mdev_register_host_device(struct device *dev,
				 const struct mdev_host_ops *ops);
void mdev_unregister_host_device(struct mdev_host *host);

int mdev_register_driver(struct mdev_driver *drv, struct module *owner);
void mdev_unregister_driver(struct mdev_driver *drv);

#endif /* MDEV_H */
