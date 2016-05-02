/*
 * VGPU definition
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef VGPU_H
#define VGPU_H

// Common Data structures

struct pci_bar_info {
	uint64_t start;
	uint64_t size;
	uint32_t flags;
};

enum vgpu_emul_space_e {
	vgpu_emul_space_config = 0, /*!< PCI configuration space */
	vgpu_emul_space_io = 1,     /*!< I/O register space */
	vgpu_emul_space_mmio = 2    /*!< Memory-mapped I/O space */
};

struct gpu_device;

/*
 * VGPU device
 */
struct vgpu_device {
	struct kref		kref;
	struct device		dev;
	struct gpu_device	*gpu_dev;
	struct iommu_group	*group;
	void			*iommu_data;
#define DEVICE_NAME_LEN		(64)
	char			dev_name[DEVICE_NAME_LEN];
	uuid_le			uuid;
	uint32_t		vgpu_instance;
	struct device_attribute	*dev_attr_vgpu_status;
	int			vgpu_device_status;

	void			*driver_data;

	struct list_head	list;
};


/**
 * struct gpu_device_ops - Structure to be registered for each physical GPU to
 * register the device to vgpu module.
 *
 * @owner:			The module owner.
 * @dev_attr_groups:		Default attributes of the physical device.
 * @vgpu_attr_groups:		Default attributes of the vGPU device.
 * @vgpu_supported_config:	Called to get information about supported vgpu types.
 *				@dev : pci device structure of physical GPU.
 *				@config: should return string listing supported config
 *				Returns integer: success (0) or error (< 0)
 * @vgpu_create:		Called to allocate basic resouces in graphics
 *				driver for a particular vgpu.
 *				@dev: physical pci device structure on which vgpu
 *				      should be created
 *				@uuid: VM's uuid for which VM it is intended to
 *				@instance: vgpu instance in that VM
 *				@vgpu_params: extra parameters required by GPU driver.
 *				Returns integer: success (0) or error (< 0)
 * @vgpu_destroy:		Called to free resources in graphics driver for
 *				a vgpu instance of that VM.
 *				@dev: physical pci device structure to which
 *				this vgpu points to.
 *				@uuid: VM's uuid for which the vgpu belongs to.
 *				@instance: vgpu instance in that VM
 *				Returns integer: success (0) or error (< 0)
 *				If VM is running and vgpu_destroy is called that
 *				means the vGPU is being hotunpluged. Return error
 *				if VM is running and graphics driver doesn't
 *				support vgpu hotplug.
 * @vgpu_start:			Called to do initiate vGPU initialization
 *				process in graphics driver when VM boots before
 *				qemu starts.
 *				@uuid: VM's UUID which is booting.
 *				Returns integer: success (0) or error (< 0)
 * @vgpu_shutdown:		Called to teardown vGPU related resources for
 *				the VM
 *				@uuid: VM's UUID which is shutting down .
 *				Returns integer: success (0) or error (< 0)
 * @read:			Read emulation callback
 *				@vdev: vgpu device structure
 *				@buf: read buffer
 *				@count: number bytes to read
 *				@address_space: specifies for which address space
 *				the request is: pci_config_space, IO register
 *				space or MMIO space.
 *				@pos: offset from base address.
 *				Retuns number on bytes read on success or error.
 * @write:			Write emulation callback
 *				@vdev: vgpu device structure
 *				@buf: write buffer
 *				@count: number bytes to be written
 *				@address_space: specifies for which address space
 *				the request is: pci_config_space, IO register
 *				space or MMIO space.
 *				@pos: offset from base address.
 *				Retuns number on bytes written on success or error.
 * @vgpu_set_irqs:		Called to send about interrupts configuration
 *				information that qemu set.
 *				@vdev: vgpu device structure
 *				@flags, index, start, count and *data : same as
 *				that of struct vfio_irq_set of
 *				VFIO_DEVICE_SET_IRQS API.
 * @vgpu_bar_info:		Called to get BAR size and flags of vGPU device.
 *				@vdev: vgpu device structure
 *				@bar_index: BAR index
 *				@bar_info: output, returns size and flags of
 *				requested BAR
 *				Returns integer: success (0) or error (< 0)
 * @validate_map_request:	Validate remap pfn request
 *				@vdev: vgpu device structure
 *				@virtaddr: target user address to start at
 *				@pfn: physical address of kernel memory, GPU
 *				driver can change if required.
 *				@size: size of map area, GPU driver can change
 *				the size of map area if desired.
 *				@prot: page protection flags for this mapping,
 *				GPU driver can change, if required.
 *				Returns integer: success (0) or error (< 0)
 *
 * Physical GPU that support vGPU should be register with vgpu module with
 * gpu_device_ops structure.
 */

struct gpu_device_ops {
	struct module   *owner;
	const struct attribute_group **dev_attr_groups;
	const struct attribute_group **vgpu_attr_groups;

	int	(*vgpu_supported_config)(struct pci_dev *dev, char *config);
	int     (*vgpu_create)(struct pci_dev *dev, uuid_le uuid,
			       uint32_t instance, char *vgpu_params);
	int     (*vgpu_destroy)(struct pci_dev *dev, uuid_le uuid,
			        uint32_t instance);

	int     (*vgpu_start)(uuid_le uuid);
	int     (*vgpu_shutdown)(uuid_le uuid);

	ssize_t (*read) (struct vgpu_device *vdev, char *buf, size_t count,
			 uint32_t address_space, loff_t pos);
	ssize_t (*write)(struct vgpu_device *vdev, char *buf, size_t count,
			 uint32_t address_space, loff_t pos);
	int     (*vgpu_set_irqs)(struct vgpu_device *vdev, uint32_t flags,
				 unsigned index, unsigned start, unsigned count,
				 void *data);
	int	(*vgpu_bar_info)(struct vgpu_device *vdev, int bar_index,
				 struct pci_bar_info *bar_info);
	int	(*validate_map_request)(struct vgpu_device *vdev,
					unsigned long virtaddr,
					unsigned long *pfn, unsigned long *size,
					pgprot_t *prot);
};

/*
 * Physical GPU
 */
struct gpu_device {
	struct pci_dev                  *dev;
	const struct gpu_device_ops     *ops;
	struct list_head                gpu_next;
};

/**
 * struct vgpu_driver - vGPU device driver
 * @name: driver name
 * @probe: called when new device created
 * @remove: called when device removed
 * @driver: device driver structure
 *
 **/
struct vgpu_driver {
	const char *name;
	int  (*probe)  (struct device *dev);
	void (*remove) (struct device *dev);
	struct device_driver	driver;
};

static inline struct vgpu_driver *to_vgpu_driver(struct device_driver *drv)
{
	return drv ? container_of(drv, struct vgpu_driver, driver) : NULL;
}

static inline struct vgpu_device *to_vgpu_device(struct device *dev)
{
	return dev ? container_of(dev, struct vgpu_device, dev) : NULL;
}

extern struct bus_type vgpu_bus_type;

#define dev_is_vgpu(d) ((d)->bus == &vgpu_bus_type)

extern int  vgpu_register_device(struct pci_dev *dev,
				 const struct gpu_device_ops *ops);
extern void vgpu_unregister_device(struct pci_dev *dev);

extern int  vgpu_register_driver(struct vgpu_driver *drv, struct module *owner);
extern void vgpu_unregister_driver(struct vgpu_driver *drv);

extern int vgpu_map_virtual_bar(uint64_t virt_bar_addr, uint64_t phys_bar_addr,
				uint32_t len, uint32_t flags);

extern struct vgpu_device *get_vgpu_device_from_group(struct iommu_group *group);

#endif /* VGPU_H */
