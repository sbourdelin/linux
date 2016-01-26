/*
 * VFIO API definition
 *
 * Copyright (C) 2012 Red Hat, Inc.  All rights reserved.
 *     Author: Alex Williamson <alex.williamson@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#ifndef VFIO_H
#define VFIO_H


#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/workqueue.h>
#include <linux/poll.h>
#include <uapi/linux/vfio.h>

struct vfio_group;

/**
 * struct vfio_device_ops - VFIO bus driver device callbacks
 *
 * @open: Called when userspace creates new file descriptor for device
 * @release: Called when userspace releases file descriptor for device
 * @read: Perform read(2) on device file descriptor
 * @write: Perform write(2) on device file descriptor
 * @ioctl: Perform ioctl(2) on device file descriptor, supporting VFIO_DEVICE_*
 *         operations documented below
 * @mmap: Perform mmap(2) on a region of the device file descriptor
 * @request: Request for the bus driver to release the device
 */
struct vfio_device_ops {
	char	*name;
	int	(*open)(void *device_data);
	void	(*release)(void *device_data);
	ssize_t	(*read)(void *device_data, char __user *buf,
			size_t count, loff_t *ppos);
	ssize_t	(*write)(void *device_data, const char __user *buf,
			 size_t count, loff_t *size);
	long	(*ioctl)(void *device_data, unsigned int cmd,
			 unsigned long arg);
	int	(*mmap)(void *device_data, struct vm_area_struct *vma);
	void	(*request)(void *device_data, unsigned int count);
	void	(*set_group)(void *device_data, struct vfio_group *group);
};

extern struct iommu_group *vfio_iommu_group_get(struct device *dev);
extern void vfio_iommu_group_put(struct iommu_group *group, struct device *dev);

extern int vfio_add_group_dev(struct device *dev,
			      const struct vfio_device_ops *ops,
			      void *device_data);

extern void *vfio_del_group_dev(struct device *dev);
extern struct vfio_device *vfio_device_get_from_dev(struct device *dev);
extern void vfio_device_put(struct vfio_device *device);
extern void *vfio_device_data(struct vfio_device *device);

/**
 * struct vfio_iommu_driver_ops - VFIO IOMMU driver callbacks
 */
struct vfio_iommu_driver_ops {
	char		*name;
	struct module	*owner;
	void		*(*open)(unsigned long arg);
	void		(*release)(void *iommu_data);
	ssize_t		(*read)(void *iommu_data, char __user *buf,
				size_t count, loff_t *ppos);
	ssize_t		(*write)(void *iommu_data, const char __user *buf,
				 size_t count, loff_t *size);
	long		(*ioctl)(void *iommu_data, unsigned int cmd,
				 unsigned long arg);
	int		(*mmap)(void *iommu_data, struct vm_area_struct *vma);
	int		(*attach_group)(void *iommu_data,
					struct iommu_group *group);
	void		(*detach_group)(void *iommu_data,
					struct iommu_group *group);
	int		(*alloc_map_reserved_iova)(void *iommu_data,
						   struct iommu_group *group,
						   phys_addr_t addr, int prot,
						   dma_addr_t *iova);
	int		(*unmap_free_reserved_iova)(void *iommu_data,
						    struct iommu_group *group,
						    dma_addr_t iova);
};

extern int vfio_register_iommu_driver(const struct vfio_iommu_driver_ops *ops);

extern void vfio_unregister_iommu_driver(
				const struct vfio_iommu_driver_ops *ops);

/*
 * External user API
 */
extern struct vfio_group *vfio_group_get_external_user(struct file *filep);
extern void vfio_group_put_external_user(struct vfio_group *group);
extern int vfio_external_user_iommu_id(struct vfio_group *group);
extern long vfio_external_check_extension(struct vfio_group *group,
					  unsigned long arg);

struct pci_dev;
#ifdef CONFIG_EEH
extern void vfio_spapr_pci_eeh_open(struct pci_dev *pdev);
extern void vfio_spapr_pci_eeh_release(struct pci_dev *pdev);
extern long vfio_spapr_iommu_eeh_ioctl(struct iommu_group *group,
				       unsigned int cmd,
				       unsigned long arg);
#else
static inline void vfio_spapr_pci_eeh_open(struct pci_dev *pdev)
{
}

static inline void vfio_spapr_pci_eeh_release(struct pci_dev *pdev)
{
}

static inline long vfio_spapr_iommu_eeh_ioctl(struct iommu_group *group,
					      unsigned int cmd,
					      unsigned long arg)
{
	return -ENOTTY;
}
#endif /* CONFIG_EEH */

/*
 * IRQfd - generic
 */
struct virqfd {
	void			*opaque;
	struct eventfd_ctx	*eventfd;
	int			(*handler)(void *, void *);
	void			(*thread)(void *, void *);
	void			*data;
	struct work_struct	inject;
	wait_queue_t		wait;
	poll_table		pt;
	struct work_struct	shutdown;
	struct virqfd		**pvirqfd;
};

extern int vfio_virqfd_enable(void *opaque,
			      int (*handler)(void *, void *),
			      void (*thread)(void *, void *),
			      void *data, struct virqfd **pvirqfd, int fd);
extern void vfio_virqfd_disable(struct virqfd **pvirqfd);

/**
 * vfio_group_alloc_map_reserved_iova: allocates a new iova page and map
 * it onto the aligned physical page that contains a given physical addr.
 * page size is the domain iommu page size.
 *
 * @group: vfio group handle
 * @addr: physical address to map
 * @prot: protection attribute
 * @iova: returned iova that is mapped onto addr
 *
 * returns 0 on success, < 0 on failure
 */
extern int vfio_group_alloc_map_reserved_iova(struct vfio_group *group,
					      phys_addr_t addr, int prot,
					      dma_addr_t *iova);
/**
 * vfio_group_unmap_free_reserved_iova: unmap and free the reserved iova page
 *
 * @group: vfio group handle
 * @iova: base iova, must be aligned on the IOMMU page size
 *
 * returns 0 on success, < 0 on failure
 */
extern int vfio_group_unmap_free_reserved_iova(struct vfio_group *group,
					       dma_addr_t iova);

#endif /* VFIO_H */
