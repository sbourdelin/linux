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

/* Common Data structures */

struct pci_region_info {
	uint64_t start;
	uint64_t size;
	uint32_t flags;		/*!< VFIO region info flags */
};

enum mdev_emul_space {
	EMUL_CONFIG_SPACE,	/*!< PCI configuration space */
	EMUL_IO,		/*!< I/O register space */
	EMUL_MMIO		/*!< Memory-mapped I/O space */
};

struct phy_device;

/*
 * Mediated device
 */

struct mdev_device {
	struct kref		kref;
	struct device		dev;
	struct phy_device	*phy_dev;
	struct iommu_group	*group;
	void			*iommu_data;
	uuid_le			uuid;
	uint32_t		instance;
	void			*driver_data;
	struct mutex		ops_lock;
	struct list_head	next;
};


/**
 * struct phy_device_ops - Structure to be registered for each physical device
 * to register the device to mdev module.
 *
 * @owner:		The module owner.
 * @dev_attr_groups:	Default attributes of the physical device.
 * @mdev_attr_groups:	Default attributes of the mediated device.
 * @supported_config:	Called to get information about supported types.
 *			@dev : device structure of physical device.
 *			@config: should return string listing supported config
 *			Returns integer: success (0) or error (< 0)
 * @create:		Called to allocate basic resources in physical device's
 *			driver for a particular mediated device
 *			@dev: physical pci device structure on which mediated
 *			      device should be created
 *			@uuid: VM's uuid for which VM it is intended to
 *			@instance: mediated instance in that VM
 *			@mdev_params: extra parameters required by physical
 *			device's driver.
 *			Returns integer: success (0) or error (< 0)
 * @destroy:		Called to free resources in physical device's driver for
 *			a mediated device instance of that VM.
 *			@dev: physical device structure to which this mediated
 *			      device points to.
 *			@uuid: VM's uuid for which the mediated device belongs
 *			@instance: mdev instance in that VM
 *			Returns integer: success (0) or error (< 0)
 *			If VM is running and destroy() is called that means the
 *			mdev is being hotunpluged. Return error if VM is running
 *			and driver doesn't support mediated device hotplug.
 * @start:		Called to do initiate mediated device initialization
 *			process in physical device's driver when VM boots before
 *			qemu starts.
 *			@uuid: VM's UUID which is booting.
 *			Returns integer: success (0) or error (< 0)
 * @shutdown:		Called to teardown mediated device related resources for
 *			the VM
 *			@uuid: VM's UUID which is shutting down .
 *			Returns integer: success (0) or error (< 0)
 * @read:		Read emulation callback
 *			@mdev: mediated device structure
 *			@buf: read buffer
 *			@count: number bytes to read
 *			@address_space: specifies for which address
 *			space the request is: pci_config_space, IO
 *			register space or MMIO space.
 *			@pos: offset from base address.
 *			Retuns number on bytes read on success or error.
 * @write:		Write emulation callback
 *			@mdev: mediated device structure
 *			@buf: write buffer
 *			@count: number bytes to be written
 *			@address_space: specifies for which address space the
 *			request is: pci_config_space, IO register space or MMIO
 *			space.
 *			@pos: offset from base address.
 *			Retuns number on bytes written on success or error.
 * @set_irqs:		Called to send about interrupts configuration
 *			information that VMM sets.
 *			@mdev: mediated device structure
 *			@flags, index, start, count and *data : same as that of
 *			struct vfio_irq_set of VFIO_DEVICE_SET_IRQS API.
 * @get_region_info:	Called to get BAR size and flags of mediated device.
 *			@mdev: mediated device structure
 *			@region_index: VFIO region index
 *			@region_info: output, returns size and flags of
 *				      requested region.
 *			Returns integer: success (0) or error (< 0)
 * @validate_map_request: Validate remap pfn request
 *			@mdev: mediated device structure
 *			@virtaddr: target user address to start at
 *			@pfn: physical address of kernel memory, vendor driver
 *			      can change if required.
 *			@size: size of map area, vendor driver can change the
 *			       size of map area if desired.
 *			@prot: page protection flags for this mapping, vendor
 *			       driver can change, if required.
 *			Returns integer: success (0) or error (< 0)
 *
 * Physical device that support mediated device should be registered with mdev
 * module with phy_device_ops structure.
 */

struct phy_device_ops {
	struct module   *owner;
	const struct attribute_group **dev_attr_groups;
	const struct attribute_group **mdev_attr_groups;

	int	(*supported_config)(struct device *dev, char *config);
	int     (*create)(struct device *dev, uuid_le uuid,
			  uint32_t instance, char *mdev_params);
	int     (*destroy)(struct device *dev, uuid_le uuid,
			   uint32_t instance);
	int     (*start)(uuid_le uuid);
	int     (*shutdown)(uuid_le uuid);
	ssize_t (*read)(struct mdev_device *vdev, char *buf, size_t count,
			enum mdev_emul_space address_space, loff_t pos);
	ssize_t (*write)(struct mdev_device *vdev, char *buf, size_t count,
			 enum mdev_emul_space address_space, loff_t pos);
	int     (*set_irqs)(struct mdev_device *vdev, uint32_t flags,
			    unsigned int index, unsigned int start,
			    unsigned int count, void *data);
	int	(*get_region_info)(struct mdev_device *vdev, int region_index,
				 struct pci_region_info *region_info);
	int	(*validate_map_request)(struct mdev_device *vdev,
					unsigned long virtaddr,
					unsigned long *pfn, unsigned long *size,
					pgprot_t *prot);
};

/*
 * Physical Device
 */
struct phy_device {
	struct device                   *dev;
	const struct phy_device_ops     *ops;
	struct list_head                next;
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

static inline struct mdev_device *mdev_get_device(struct mdev_device *vdev)
{
	return (vdev && get_device(&vdev->dev)) ? vdev : NULL;
}

static inline  void mdev_put_device(struct mdev_device *vdev)
{
	if (vdev)
		put_device(&vdev->dev);
}

extern struct bus_type mdev_bus_type;

#define dev_is_mdev(d) ((d)->bus == &mdev_bus_type)

extern int  mdev_register_device(struct device *dev,
				 const struct phy_device_ops *ops);
extern void mdev_unregister_device(struct device *dev);

extern int  mdev_register_driver(struct mdev_driver *drv, struct module *owner);
extern void mdev_unregister_driver(struct mdev_driver *drv);

extern int mdev_map_virtual_bar(uint64_t virt_bar_addr, uint64_t phys_bar_addr,
				uint32_t len, uint32_t flags);

extern struct mdev_device *mdev_get_device_by_group(struct iommu_group *group);

#endif /* MDEV_H */
