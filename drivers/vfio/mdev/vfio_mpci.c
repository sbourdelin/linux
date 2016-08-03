/*
 * VFIO based Mediated PCI device driver
 *
 * Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
 *     Author: Neo Jia <cjia@nvidia.com>
 *	       Kirti Wankhede <kwankhede@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uuid.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/mdev.h>

#include "mdev_private.h"

#define DRIVER_VERSION  "0.1"
#define DRIVER_AUTHOR   "NVIDIA Corporation"
#define DRIVER_DESC     "VFIO based Mediated PCI device driver"

struct vfio_mdev {
	struct iommu_group *group;
	struct mdev_device *mdev;
	int		    refcnt;
	struct vfio_region_info vfio_region_info[VFIO_PCI_NUM_REGIONS];
	struct mutex	    vfio_mdev_lock;
};

static int vfio_mpci_open(void *device_data)
{
	int ret = 0;
	struct vfio_mdev *vmdev = device_data;
	struct parent_device *parent = vmdev->mdev->parent;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	mutex_lock(&vmdev->vfio_mdev_lock);
	if (!vmdev->refcnt && parent->ops->get_region_info) {
		int index;

		for (index = VFIO_PCI_BAR0_REGION_INDEX;
		     index < VFIO_PCI_NUM_REGIONS; index++) {
			ret = parent->ops->get_region_info(vmdev->mdev, index,
					      &vmdev->vfio_region_info[index]);
			if (ret)
				goto open_error;
		}
	}

	vmdev->refcnt++;

open_error:
	mutex_unlock(&vmdev->vfio_mdev_lock);
	if (ret)
		module_put(THIS_MODULE);

	return ret;
}

static void vfio_mpci_close(void *device_data)
{
	struct vfio_mdev *vmdev = device_data;

	mutex_lock(&vmdev->vfio_mdev_lock);
	vmdev->refcnt--;
	if (!vmdev->refcnt) {
		memset(&vmdev->vfio_region_info, 0,
			sizeof(vmdev->vfio_region_info));
	}
	mutex_unlock(&vmdev->vfio_mdev_lock);
	module_put(THIS_MODULE);
}

static u8 mpci_find_pci_capability(struct mdev_device *mdev, u8 capability)
{
	loff_t pos = VFIO_PCI_INDEX_TO_OFFSET(VFIO_PCI_CONFIG_REGION_INDEX);
	struct parent_device *parent = mdev->parent;
	u16 status;
	u8  cap_ptr, cap_id = 0xff;

	parent->ops->read(mdev, (char *)&status, sizeof(status),
			  pos + PCI_STATUS);
	if (!(status & PCI_STATUS_CAP_LIST))
		return 0;

	parent->ops->read(mdev, &cap_ptr, sizeof(cap_ptr),
			  pos + PCI_CAPABILITY_LIST);

	do {
		cap_ptr &= 0xfc;
		parent->ops->read(mdev, &cap_id, sizeof(cap_id),
				  pos + cap_ptr + PCI_CAP_LIST_ID);
		if (cap_id == capability)
			return cap_ptr;
		parent->ops->read(mdev, &cap_ptr, sizeof(cap_ptr),
				  pos + cap_ptr + PCI_CAP_LIST_NEXT);
	} while (cap_ptr && cap_id != 0xff);

	return 0;
}

static int mpci_get_irq_count(struct vfio_mdev *vmdev, int irq_type)
{
	loff_t pos = VFIO_PCI_INDEX_TO_OFFSET(VFIO_PCI_CONFIG_REGION_INDEX);
	struct mdev_device *mdev = vmdev->mdev;
	struct parent_device *parent = mdev->parent;

	if (irq_type == VFIO_PCI_INTX_IRQ_INDEX) {
		u8 pin;

		parent->ops->read(mdev, &pin, sizeof(pin),
				  pos + PCI_INTERRUPT_PIN);
		if (IS_ENABLED(CONFIG_VFIO_PCI_INTX) && pin)
			return 1;

	} else if (irq_type == VFIO_PCI_MSI_IRQ_INDEX) {
		u8 cap_ptr;
		u16 flags;

		cap_ptr = mpci_find_pci_capability(mdev, PCI_CAP_ID_MSI);
		if (cap_ptr) {
			parent->ops->read(mdev, (char *)&flags, sizeof(flags),
					pos + cap_ptr + PCI_MSI_FLAGS);
			return 1 << ((flags & PCI_MSI_FLAGS_QMASK) >> 1);
		}
	} else if (irq_type == VFIO_PCI_MSIX_IRQ_INDEX) {
		u8 cap_ptr;
		u16 flags;

		cap_ptr = mpci_find_pci_capability(mdev, PCI_CAP_ID_MSIX);
		if (cap_ptr) {
			parent->ops->read(mdev, (char *)&flags, sizeof(flags),
					pos + cap_ptr + PCI_MSIX_FLAGS);

			return (flags & PCI_MSIX_FLAGS_QSIZE) + 1;
		}
	} else if (irq_type == VFIO_PCI_ERR_IRQ_INDEX) {
		u8 cap_ptr;

		cap_ptr = mpci_find_pci_capability(mdev, PCI_CAP_ID_EXP);
		if (cap_ptr)
			return 1;
	} else if (irq_type == VFIO_PCI_REQ_IRQ_INDEX) {
		return 1;
	}

	return 0;
}

static long vfio_mpci_unlocked_ioctl(void *device_data,
				     unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct vfio_mdev *vmdev = device_data;
	unsigned long minsz;

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO:
	{
		struct vfio_device_info info;
		struct parent_device *parent = vmdev->mdev->parent;

		minsz = offsetofend(struct vfio_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = VFIO_DEVICE_FLAGS_PCI;

		if (parent->ops->reset)
			info.flags |= VFIO_DEVICE_FLAGS_RESET;

		info.num_regions = VFIO_PCI_NUM_REGIONS;
		info.num_irqs = VFIO_PCI_NUM_IRQS;

		return copy_to_user((void __user *)arg, &info, minsz);
	}
	case VFIO_DEVICE_GET_REGION_INFO:
	{
		struct vfio_region_info info;

		minsz = offsetofend(struct vfio_region_info, offset);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		switch (info.index) {
		case VFIO_PCI_CONFIG_REGION_INDEX:
		case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = vmdev->vfio_region_info[info.index].size;
			if (!info.size) {
				info.flags = 0;
				break;
			}

			info.flags = vmdev->vfio_region_info[info.index].flags;
			break;
		case VFIO_PCI_VGA_REGION_INDEX:
		case VFIO_PCI_ROM_REGION_INDEX:
		default:
			return -EINVAL;
		}

		return copy_to_user((void __user *)arg, &info, minsz);
	}
	case VFIO_DEVICE_GET_IRQ_INFO:
	{
		struct vfio_irq_info info;

		minsz = offsetofend(struct vfio_irq_info, count);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz || info.index >= VFIO_PCI_NUM_IRQS)
			return -EINVAL;

		switch (info.index) {
		case VFIO_PCI_INTX_IRQ_INDEX ... VFIO_PCI_MSI_IRQ_INDEX:
		case VFIO_PCI_REQ_IRQ_INDEX:
			break;
			/* pass thru to return error */
		case VFIO_PCI_MSIX_IRQ_INDEX:
		default:
			return -EINVAL;
		}

		info.flags = VFIO_IRQ_INFO_EVENTFD;
		info.count = mpci_get_irq_count(vmdev, info.index);

		if (info.count == -1)
			return -EINVAL;

		if (info.index == VFIO_PCI_INTX_IRQ_INDEX)
			info.flags |= (VFIO_IRQ_INFO_MASKABLE |
					VFIO_IRQ_INFO_AUTOMASKED);
		else
			info.flags |= VFIO_IRQ_INFO_NORESIZE;

		return copy_to_user((void __user *)arg, &info, minsz);
	}
	case VFIO_DEVICE_SET_IRQS:
	{
		struct vfio_irq_set hdr;
		struct mdev_device *mdev = vmdev->mdev;
		struct parent_device *parent = mdev->parent;
		u8 *data = NULL, *ptr = NULL;

		minsz = offsetofend(struct vfio_irq_set, count);

		if (copy_from_user(&hdr, (void __user *)arg, minsz))
			return -EFAULT;

		if (hdr.argsz < minsz || hdr.index >= VFIO_PCI_NUM_IRQS ||
		    hdr.flags & ~(VFIO_IRQ_SET_DATA_TYPE_MASK |
				  VFIO_IRQ_SET_ACTION_TYPE_MASK))
			return -EINVAL;

		if (!(hdr.flags & VFIO_IRQ_SET_DATA_NONE)) {
			size_t size;
			int max = mpci_get_irq_count(vmdev, hdr.index);

			if (hdr.flags & VFIO_IRQ_SET_DATA_BOOL)
				size = sizeof(uint8_t);
			else if (hdr.flags & VFIO_IRQ_SET_DATA_EVENTFD)
				size = sizeof(int32_t);
			else
				return -EINVAL;

			if (hdr.argsz - minsz < hdr.count * size ||
			    hdr.start >= max || hdr.start + hdr.count > max)
				return -EINVAL;

			ptr = data = memdup_user((void __user *)(arg + minsz),
						 hdr.count * size);
			if (IS_ERR(data))
				return PTR_ERR(data);
		}

		if (parent->ops->set_irqs)
			ret = parent->ops->set_irqs(mdev, hdr.flags, hdr.index,
						    hdr.start, hdr.count, data);

		kfree(ptr);
		return ret;
	}
	case VFIO_DEVICE_RESET:
	{
		struct parent_device *parent = vmdev->mdev->parent;

		if (parent->ops->reset)
			return parent->ops->reset(vmdev->mdev);

		return -EINVAL;
	}
	}
	return -ENOTTY;
}

static ssize_t vfio_mpci_read(void *device_data, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct vfio_mdev *vmdev = device_data;
	struct mdev_device *mdev = vmdev->mdev;
	struct parent_device *parent = mdev->parent;
	int ret = 0;

	if (!count)
		return 0;

	if (parent->ops->read) {
		char *ret_data, *ptr;

		ptr = ret_data = kzalloc(count, GFP_KERNEL);

		if (!ret_data)
			return  -ENOMEM;

		ret = parent->ops->read(mdev, ret_data, count, *ppos);

		if (ret > 0) {
			if (copy_to_user(buf, ret_data, ret))
				ret = -EFAULT;
			else
				*ppos += ret;
		}
		kfree(ptr);
	}

	return ret;
}

static ssize_t vfio_mpci_write(void *device_data, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct vfio_mdev *vmdev = device_data;
	struct mdev_device *mdev = vmdev->mdev;
	struct parent_device *parent = mdev->parent;
	int ret = 0;

	if (!count)
		return 0;

	if (parent->ops->write) {
		char *usr_data, *ptr;

		ptr = usr_data = memdup_user(buf, count);
		if (IS_ERR(usr_data))
			return PTR_ERR(usr_data);

		ret = parent->ops->write(mdev, usr_data, count, *ppos);

		if (ret > 0)
			*ppos += ret;

		kfree(ptr);
	}

	return ret;
}

static int mdev_dev_mmio_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret;
	struct vfio_mdev *vmdev = vma->vm_private_data;
	struct mdev_device *mdev;
	struct parent_device *parent;
	u64 virtaddr = (u64)vmf->virtual_address;
	unsigned long req_size, pgoff = 0;
	pgprot_t pg_prot;
	unsigned int index;

	if (!vmdev && !vmdev->mdev)
		return -EINVAL;

	mdev = vmdev->mdev;
	parent  = mdev->parent;

	pg_prot  = vma->vm_page_prot;

	if (parent->ops->validate_map_request) {
		u64 offset;
		loff_t pos;

		offset   = virtaddr - vma->vm_start;
		req_size = vma->vm_end - virtaddr;
		pos = (vma->vm_pgoff << PAGE_SHIFT) + offset;

		ret = parent->ops->validate_map_request(mdev, pos, &virtaddr,
						&pgoff, &req_size, &pg_prot);
		if (ret)
			return ret;

		/*
		 * Verify pgoff and req_size are valid and virtaddr is within
		 * vma range
		 */
		if (!pgoff || !req_size || (virtaddr < vma->vm_start) ||
		    ((virtaddr + req_size) >= vma->vm_end))
			return -EINVAL;
	} else {
		struct pci_dev *pdev;

		virtaddr = vma->vm_start;
		req_size = vma->vm_end - vma->vm_start;

		pdev = to_pci_dev(parent->dev);
		index = VFIO_PCI_OFFSET_TO_INDEX(vma->vm_pgoff << PAGE_SHIFT);
		pgoff = pci_resource_start(pdev, index) >> PAGE_SHIFT;
	}

	ret = remap_pfn_range(vma, virtaddr, pgoff, req_size, pg_prot);

	return ret | VM_FAULT_NOPAGE;
}

void mdev_dev_mmio_close(struct vm_area_struct *vma)
{
	struct vfio_mdev *vmdev = vma->vm_private_data;
	struct mdev_device *mdev = vmdev->mdev;

	mdev_del_phys_mapping(mdev, vma->vm_pgoff << PAGE_SHIFT);
}

static const struct vm_operations_struct mdev_dev_mmio_ops = {
	.fault = mdev_dev_mmio_fault,
	.close = mdev_dev_mmio_close,
};

static int vfio_mpci_mmap(void *device_data, struct vm_area_struct *vma)
{
	unsigned int index;
	struct vfio_mdev *vmdev = device_data;
	struct mdev_device *mdev = vmdev->mdev;

	index = vma->vm_pgoff >> (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT);

	if (index >= VFIO_PCI_ROM_REGION_INDEX)
		return -EINVAL;

	vma->vm_private_data = vmdev;
	vma->vm_ops = &mdev_dev_mmio_ops;

	return mdev_add_phys_mapping(mdev, vma->vm_file->f_mapping,
				     vma->vm_pgoff << PAGE_SHIFT,
				     vma->vm_end - vma->vm_start);
}

static const struct vfio_device_ops vfio_mpci_dev_ops = {
	.name		= "vfio-mpci",
	.open		= vfio_mpci_open,
	.release	= vfio_mpci_close,
	.ioctl		= vfio_mpci_unlocked_ioctl,
	.read		= vfio_mpci_read,
	.write		= vfio_mpci_write,
	.mmap		= vfio_mpci_mmap,
};

int vfio_mpci_probe(struct device *dev)
{
	struct vfio_mdev *vmdev;
	struct mdev_device *mdev = to_mdev_device(dev);
	int ret;

	vmdev = kzalloc(sizeof(*vmdev), GFP_KERNEL);
	if (IS_ERR(vmdev))
		return PTR_ERR(vmdev);

	vmdev->mdev = mdev_get_device(mdev);
	vmdev->group = mdev->group;
	mutex_init(&vmdev->vfio_mdev_lock);

	ret = vfio_add_group_dev(dev, &vfio_mpci_dev_ops, vmdev);
	if (ret)
		kfree(vmdev);

	mdev_put_device(mdev);
	return ret;
}

void vfio_mpci_remove(struct device *dev)
{
	struct vfio_mdev *vmdev;

	vmdev = vfio_del_group_dev(dev);
	kfree(vmdev);
}

int vfio_mpci_match(struct device *dev)
{
	if (dev_is_pci(dev->parent))
		return 1;

	return 0;
}

struct mdev_driver vfio_mpci_driver = {
	.name	= "vfio_mpci",
	.probe	= vfio_mpci_probe,
	.remove	= vfio_mpci_remove,
	.match	= vfio_mpci_match,
};

static int __init vfio_mpci_init(void)
{
	return mdev_register_driver(&vfio_mpci_driver, THIS_MODULE);
}

static void __exit vfio_mpci_exit(void)
{
	mdev_unregister_driver(&vfio_mpci_driver);
}

module_init(vfio_mpci_init)
module_exit(vfio_mpci_exit)

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
