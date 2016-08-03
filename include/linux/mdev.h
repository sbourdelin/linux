/*
 * Mediated device definition
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef MDEV_H
#define MDEV_H

#include <uapi/linux/vfio.h>

struct parent_device;

/*
 * Mediated device
 */

struct addr_desc {
	unsigned long start;
	unsigned long size;
	struct list_head next;
};

struct mdev_phys_mapping {
	struct address_space *mapping;
	struct list_head addr_desc_list;
	struct mutex addr_desc_list_lock;
};

struct mdev_device {
	struct device		dev;
	struct parent_device	*parent;
	struct iommu_group	*group;
	uuid_le			uuid;
	uint32_t		instance;
	void			*driver_data;

	/* internal only */
	struct kref		ref;
	struct list_head	next;

	struct mdev_phys_mapping phys_mappings;
};


/**
 * struct parent_ops - Structure to be registered for each parent device to
 * register the device to mdev module.
 *
 * @owner:		The module owner.
 * @dev_attr_groups:	Default attributes of the parent device.
 * @mdev_attr_groups:	Default attributes of the mediated device.
 * @supported_config:	Called to get information about supported types.
 *			@dev : device structure of parent device.
 *			@config: should return string listing supported config
 *			Returns integer: success (0) or error (< 0)
 * @create:		Called to allocate basic resources in parent device's
 *			driver for a particular mediated device. It is
 *			mandatory to provide create ops.
 *			@mdev: mdev_device structure on of mediated device
 *			      that is being created
 *			@mdev_params: extra parameters required by parent
 *			device's driver.
 *			Returns integer: success (0) or error (< 0)
 * @destroy:		Called to free resources in parent device's driver for a
 *			a mediated device instance. It is mandatory to provide
 *			destroy ops.
 *			@mdev: mdev_device device structure which is being
 *			       destroyed
 *			Returns integer: success (0) or error (< 0)
 *			If VMM is running and destroy() is called that means the
 *			mdev is being hotunpluged. Return error if VMM is
 *			running and driver doesn't support mediated device
 *			hotplug.
 * @reset:		Called to reset mediated device.
 *			@mdev: mdev_device device structure
 *			Returns integer: success (0) or error (< 0)
 * @start:		Called to initiate mediated device initialization
 *			process in parent device's driver before VMM starts.
 *			@uuid: UUID
 *			Returns integer: success (0) or error (< 0)
 * @stop:		Called to teardown mediated device related resources
 *			@uuid: UUID
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
 * @set_irqs:		Called to send about interrupts configuration
 *			information that VMM sets.
 *			@mdev: mediated device structure
 *			@flags, index, start, count and *data : same as that of
 *			struct vfio_irq_set of VFIO_DEVICE_SET_IRQS API.
 * @get_region_info:	Called to get VFIO region size and flags of mediated
 *			device.
 *			@mdev: mediated device structure
 *			@region_index: VFIO region index
 *			@region_info: output, returns size and flags of
 *				      requested region.
 *			Returns integer: success (0) or error (< 0)
 * @validate_map_request: Validate remap pfn request
 *			@mdev: mediated device structure
 *			@pos: address
 *			@virtaddr: target user address to start at. Vendor
 *				   driver can change if required.
 *			@pfn: parent address of kernel memory, vendor driver
 *			      can change if required.
 *			@size: size of map area, vendor driver can change the
 *			       size of map area if desired.
 *			@prot: page protection flags for this mapping, vendor
 *			       driver can change, if required.
 *			Returns integer: success (0) or error (< 0)
 *
 * Parent device that support mediated device should be registered with mdev
 * module with parent_ops structure.
 */

struct parent_ops {
	struct module   *owner;
	const struct attribute_group **dev_attr_groups;
	const struct attribute_group **mdev_attr_groups;

	int	(*supported_config)(struct device *dev, char *config);
	int     (*create)(struct mdev_device *mdev, char *mdev_params);
	int     (*destroy)(struct mdev_device *mdev);
	int     (*reset)(struct mdev_device *mdev);
	int     (*start)(uuid_le uuid);
	int     (*stop)(uuid_le uuid);
	ssize_t (*read)(struct mdev_device *mdev, char *buf, size_t count,
			loff_t pos);
	ssize_t (*write)(struct mdev_device *mdev, char *buf, size_t count,
			 loff_t pos);
	int     (*set_irqs)(struct mdev_device *mdev, uint32_t flags,
			    unsigned int index, unsigned int start,
			    unsigned int count, void *data);
	int	(*get_region_info)(struct mdev_device *mdev, int region_index,
				   struct vfio_region_info *region_info);
	int	(*validate_map_request)(struct mdev_device *mdev, loff_t pos,
					u64 *virtaddr, unsigned long *pfn,
					unsigned long *size, pgprot_t *prot);
};

/*
 * Parent Device
 */

struct parent_device {
	struct device		*dev;
	const struct parent_ops	*ops;

	/* internal */
	struct kref		ref;
	struct list_head	next;
	struct list_head	mdev_list;
	struct mutex		mdev_list_lock;
	wait_queue_head_t	release_done;
};

/**
 * struct mdev_driver - Mediated device driver
 * @name: driver name
 * @probe: called when new device created
 * @remove: called when device removed
 * @match: called when new device or driver is added for this bus. Return 1 if
 *	   given device can be handled by given driver and zero otherwise.
 * @driver: device driver structure
 *
 **/
struct mdev_driver {
	const char *name;
	int  (*probe)(struct device *dev);
	void (*remove)(struct device *dev);
	int  (*match)(struct device *dev);
	struct device_driver driver;
};

static inline struct mdev_driver *to_mdev_driver(struct device_driver *drv)
{
	return drv ? container_of(drv, struct mdev_driver, driver) : NULL;
}

static inline struct mdev_device *to_mdev_device(struct device *dev)
{
	return dev ? container_of(dev, struct mdev_device, dev) : NULL;
}

static inline void *mdev_get_drvdata(struct mdev_device *mdev)
{
	return mdev->driver_data;
}

static inline void mdev_set_drvdata(struct mdev_device *mdev, void *data)
{
	mdev->driver_data = data;
}

extern struct bus_type mdev_bus_type;

#define dev_is_mdev(d) ((d)->bus == &mdev_bus_type)

extern int  mdev_register_device(struct device *dev,
				 const struct parent_ops *ops);
extern void mdev_unregister_device(struct device *dev);

extern int  mdev_register_driver(struct mdev_driver *drv, struct module *owner);
extern void mdev_unregister_driver(struct mdev_driver *drv);

extern struct mdev_device *mdev_get_device(struct mdev_device *mdev);
extern void mdev_put_device(struct mdev_device *mdev);

extern struct mdev_device *mdev_get_device_by_group(struct iommu_group *group);

extern int mdev_device_invalidate_mapping(struct mdev_device *mdev,
					unsigned long addr, unsigned long size);

extern int mdev_add_phys_mapping(struct mdev_device *mdev,
				 struct address_space *mapping,
				 unsigned long addr, unsigned long size);


extern void mdev_del_phys_mapping(struct mdev_device *mdev, unsigned long addr);
#endif /* MDEV_H */
