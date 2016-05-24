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

struct vfio_mdevice {
	struct iommu_group *group;
	struct mdev_device *mdevice;
	int		    refcnt;
	struct pci_region_info vfio_region_info[VFIO_PCI_NUM_REGIONS];
	u8		    *vconfig;
	struct mutex	    vfio_mdev_lock;
};

static int get_virtual_bar_info(struct mdev_device *mdevice,
				struct pci_region_info *vfio_region_info,
				int index)
{
	int ret = -EINVAL;
	struct phy_device *phy_dev = mdevice->phy_dev;

	if (dev_is_pci(phy_dev->dev) && phy_dev->ops->get_region_info) {
		mutex_lock(&mdevice->ops_lock);
		ret = phy_dev->ops->get_region_info(mdevice, index,
						    vfio_region_info);
		mutex_unlock(&mdevice->ops_lock);
	}
	return ret;
}

static int mdev_read_base(struct vfio_mdevice *vdev)
{
	int index, pos;
	u32 start_lo, start_hi;
	u32 mem_type;

	pos = PCI_BASE_ADDRESS_0;

	for (index = 0; index <= VFIO_PCI_BAR5_REGION_INDEX; index++) {

		if (!vdev->vfio_region_info[index].size)
			continue;

		start_lo = (*(u32 *)(vdev->vconfig + pos)) &
					PCI_BASE_ADDRESS_MEM_MASK;
		mem_type = (*(u32 *)(vdev->vconfig + pos)) &
					PCI_BASE_ADDRESS_MEM_TYPE_MASK;

		switch (mem_type) {
		case PCI_BASE_ADDRESS_MEM_TYPE_64:
			start_hi = (*(u32 *)(vdev->vconfig + pos + 4));
			pos += 4;
			break;
		case PCI_BASE_ADDRESS_MEM_TYPE_32:
		case PCI_BASE_ADDRESS_MEM_TYPE_1M:
			/* 1M mem BAR treated as 32-bit BAR */
		default:
			/* mem unknown type treated as 32-bit BAR */
			start_hi = 0;
			break;
		}
		pos += 4;
		vdev->vfio_region_info[index].start = ((u64)start_hi << 32) |
							start_lo;
	}
	return 0;
}

static int vfio_mpci_open(void *device_data)
{
	int ret = 0;
	struct vfio_mdevice *vdev = device_data;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	mutex_lock(&vdev->vfio_mdev_lock);
	if (!vdev->refcnt) {
		u8 *vconfig;
		int index;
		struct pci_region_info *cfg_reg;

		for (index = VFIO_PCI_BAR0_REGION_INDEX;
		     index < VFIO_PCI_NUM_REGIONS; index++) {
			ret = get_virtual_bar_info(vdev->mdevice,
						&vdev->vfio_region_info[index],
						index);
			if (ret)
				goto open_error;
		}
		cfg_reg = &vdev->vfio_region_info[VFIO_PCI_CONFIG_REGION_INDEX];
		if (!cfg_reg->size)
			goto open_error;

		vconfig = kzalloc(cfg_reg->size, GFP_KERNEL);
		if (IS_ERR(vconfig)) {
			ret = PTR_ERR(vconfig);
			goto open_error;
		}

		vdev->vconfig = vconfig;
	}

	vdev->refcnt++;
open_error:

	mutex_unlock(&vdev->vfio_mdev_lock);
	if (ret)
		module_put(THIS_MODULE);

	return ret;
}

static void vfio_mpci_close(void *device_data)
{
	struct vfio_mdevice *vdev = device_data;

	mutex_lock(&vdev->vfio_mdev_lock);
	vdev->refcnt--;
	if (!vdev->refcnt) {
		memset(&vdev->vfio_region_info, 0,
			sizeof(vdev->vfio_region_info));
		kfree(vdev->vconfig);
	}
	mutex_unlock(&vdev->vfio_mdev_lock);
}

static int mdev_get_irq_count(struct vfio_mdevice *vdev, int irq_type)
{
	/* Don't support MSIX for now */
	if (irq_type == VFIO_PCI_MSIX_IRQ_INDEX)
		return -1;

	return 1;
}

static long vfio_mpci_unlocked_ioctl(void *device_data,
				     unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct vfio_mdevice *vdev = device_data;
	unsigned long minsz;

	switch (cmd) {
	case VFIO_DEVICE_GET_INFO:
	{
		struct vfio_device_info info;

		minsz = offsetofend(struct vfio_device_info, num_irqs);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = VFIO_DEVICE_FLAGS_PCI;
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
			info.size = vdev->vfio_region_info[info.index].size;
			if (!info.size) {
				info.flags = 0;
				break;
			}

			info.flags = vdev->vfio_region_info[info.index].flags;
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

		info.count = VFIO_PCI_NUM_IRQS;

		info.flags = VFIO_IRQ_INFO_EVENTFD;
		info.count = mdev_get_irq_count(vdev, info.index);

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
		struct mdev_device *mdevice = vdev->mdevice;
		struct phy_device *phy_dev = vdev->mdevice->phy_dev;
		u8 *data = NULL;
		int ret = 0;

		minsz = offsetofend(struct vfio_irq_set, count);

		if (copy_from_user(&hdr, (void __user *)arg, minsz))
			return -EFAULT;

		if (hdr.argsz < minsz || hdr.index >= VFIO_PCI_NUM_IRQS ||
		    hdr.flags & ~(VFIO_IRQ_SET_DATA_TYPE_MASK |
		    VFIO_IRQ_SET_ACTION_TYPE_MASK))
			return -EINVAL;

		if (!(hdr.flags & VFIO_IRQ_SET_DATA_NONE)) {
			size_t size;
			int max = mdev_get_irq_count(vdev, hdr.index);

			if (hdr.flags & VFIO_IRQ_SET_DATA_BOOL)
				size = sizeof(uint8_t);
			else if (hdr.flags & VFIO_IRQ_SET_DATA_EVENTFD)
				size = sizeof(int32_t);
			else
				return -EINVAL;

			if (hdr.argsz - minsz < hdr.count * size ||
			    hdr.start >= max || hdr.start + hdr.count > max)
				return -EINVAL;

			data = memdup_user((void __user *)(arg + minsz),
					    hdr.count * size);
			if (IS_ERR(data))
				return PTR_ERR(data);

			}

			if (phy_dev->ops->set_irqs) {
				mutex_lock(&mdevice->ops_lock);
				ret = phy_dev->ops->set_irqs(mdevice, hdr.flags,
							   hdr.index, hdr.start,
							   hdr.count, data);
				mutex_unlock(&mdevice->ops_lock);
			}

			kfree(data);
			return ret;
	}

	default:
		return -EINVAL;
	}
	return ret;
}

ssize_t mdev_dev_config_rw(struct vfio_mdevice *vdev, char __user *buf,
			   size_t count, loff_t *ppos, bool iswrite)
{
	struct mdev_device *mdevice = vdev->mdevice;
	struct phy_device *phy_dev = mdevice->phy_dev;
	int size = vdev->vfio_region_info[VFIO_PCI_CONFIG_REGION_INDEX].size;
	int ret = 0;
	uint64_t pos = *ppos & VFIO_PCI_OFFSET_MASK;

	if (pos < 0 || pos >= size ||
	    pos + count > size) {
		pr_err("%s pos 0x%llx out of range\n", __func__, pos);
		ret = -EFAULT;
		goto config_rw_exit;
	}

	if (iswrite) {
		char *user_data;

		user_data = memdup_user(buf, count);
		if (IS_ERR(user_data)) {
			ret = PTR_ERR(user_data);
			goto config_rw_exit;
		}

		if (phy_dev->ops->write) {
			mutex_lock(&mdevice->ops_lock);
			ret = phy_dev->ops->write(mdevice, user_data, count,
						  EMUL_CONFIG_SPACE, pos);
			mutex_unlock(&mdevice->ops_lock);
		}

		memcpy((void *)(vdev->vconfig + pos), (void *)user_data, count);
		kfree(user_data);
	} else {
		char *ret_data = kzalloc(count, GFP_KERNEL);

		if (IS_ERR(ret_data)) {
			ret = PTR_ERR(ret_data);
			goto config_rw_exit;
		}

		if (phy_dev->ops->read) {
			mutex_lock(&mdevice->ops_lock);
			ret = phy_dev->ops->read(mdevice, ret_data, count,
						 EMUL_CONFIG_SPACE, pos);
			mutex_unlock(&mdevice->ops_lock);
		}

		if (ret > 0) {
			if (copy_to_user(buf, ret_data, ret)) {
				ret = -EFAULT;
				kfree(ret_data);
				goto config_rw_exit;
			}

			memcpy((void *)(vdev->vconfig + pos),
				(void *)ret_data, count);
		}
		kfree(ret_data);
	}
config_rw_exit:
	return ret;
}

ssize_t mdev_dev_bar_rw(struct vfio_mdevice *vdev, char __user *buf,
			size_t count, loff_t *ppos, bool iswrite)
{
	struct mdev_device *mdevice = vdev->mdevice;
	struct phy_device *phy_dev = mdevice->phy_dev;
	loff_t offset = *ppos & VFIO_PCI_OFFSET_MASK;
	loff_t pos;
	int bar_index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	int ret = 0;

	if (!vdev->vfio_region_info[bar_index].start) {
		ret = mdev_read_base(vdev);
		if (ret)
			goto bar_rw_exit;
	}

	if (offset >= vdev->vfio_region_info[bar_index].size) {
		ret = -EINVAL;
		goto bar_rw_exit;
	}

	pos = vdev->vfio_region_info[bar_index].start + offset;
	if (iswrite) {
		char *user_data;

		user_data = memdup_user(buf, count);
		if (IS_ERR(user_data)) {
			ret = PTR_ERR(user_data);
			goto bar_rw_exit;
		}

		if (phy_dev->ops->write) {
			mutex_lock(&mdevice->ops_lock);
			ret = phy_dev->ops->write(mdevice,  user_data, count,
						  EMUL_MMIO, pos);
			mutex_unlock(&mdevice->ops_lock);
		}

		kfree(user_data);
	} else {
		char *ret_data = kzalloc(count, GFP_KERNEL);

		if (IS_ERR(ret_data)) {
			ret = PTR_ERR(ret_data);
			goto bar_rw_exit;
		}

		memset(ret_data, 0, count);

		if (phy_dev->ops->read) {
			mutex_lock(&mdevice->ops_lock);
			ret = phy_dev->ops->read(mdevice, ret_data, count,
						 EMUL_MMIO, pos);
			mutex_unlock(&mdevice->ops_lock);
		}

		if (ret > 0) {
			if (copy_to_user(buf, ret_data, ret))
				ret = -EFAULT;
		}
		kfree(ret_data);
	}

bar_rw_exit:
	return ret;
}


static ssize_t mdev_dev_rw(void *device_data, char __user *buf,
			   size_t count, loff_t *ppos, bool iswrite)
{
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	struct vfio_mdevice *vdev = device_data;

	if (index >= VFIO_PCI_NUM_REGIONS)
		return -EINVAL;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		return mdev_dev_config_rw(vdev, buf, count, ppos, iswrite);

	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		return mdev_dev_bar_rw(vdev, buf, count, ppos, iswrite);

	case VFIO_PCI_ROM_REGION_INDEX:
	case VFIO_PCI_VGA_REGION_INDEX:
		break;
	}

	return -EINVAL;
}


static ssize_t vfio_mpci_read(void *device_data, char __user *buf,
			      size_t count, loff_t *ppos)
{
	int ret = 0;

	if (count)
		ret = mdev_dev_rw(device_data, buf, count, ppos, false);

	return ret;
}

static ssize_t vfio_mpci_write(void *device_data, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	int ret = 0;

	if (count)
		ret = mdev_dev_rw(device_data, (char *)buf, count, ppos, true);

	return ret;
}

static int mdev_dev_mmio_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret = 0;
	struct vfio_mdevice *vdev = vma->vm_private_data;
	struct mdev_device *mdevice;
	struct phy_device *phy_dev;
	u64 virtaddr = (u64)vmf->virtual_address;
	u64 offset, phyaddr;
	unsigned long req_size, pgoff;
	pgprot_t pg_prot;

	if (!vdev && !vdev->mdevice)
		return -EINVAL;

	mdevice = vdev->mdevice;
	phy_dev  = mdevice->phy_dev;

	offset   = vma->vm_pgoff << PAGE_SHIFT;
	phyaddr  = virtaddr - vma->vm_start + offset;
	pgoff    = phyaddr >> PAGE_SHIFT;
	req_size = vma->vm_end - virtaddr;
	pg_prot  = vma->vm_page_prot;

	if (phy_dev->ops->validate_map_request) {
		mutex_lock(&mdevice->ops_lock);
		ret = phy_dev->ops->validate_map_request(mdevice, virtaddr,
							 &pgoff, &req_size,
							 &pg_prot);
		mutex_unlock(&mdevice->ops_lock);
		if (ret)
			return ret;

		if (!req_size)
			return -EINVAL;
	}

	ret = remap_pfn_range(vma, virtaddr, pgoff, req_size, pg_prot);

	return ret | VM_FAULT_NOPAGE;
}

static const struct vm_operations_struct mdev_dev_mmio_ops = {
	.fault = mdev_dev_mmio_fault,
};


static int vfio_mpci_mmap(void *device_data, struct vm_area_struct *vma)
{
	unsigned int index;
	struct vfio_mdevice *vdev = device_data;
	struct mdev_device *mdevice = vdev->mdevice;
	struct pci_dev *pdev;
	unsigned long pgoff;
	loff_t offset;

	if (!dev_is_pci(mdevice->phy_dev->dev))
		return -EINVAL;

	pdev = to_pci_dev(mdevice->phy_dev->dev);

	offset = vma->vm_pgoff << PAGE_SHIFT;

	index = VFIO_PCI_OFFSET_TO_INDEX(offset);

	if (index >= VFIO_PCI_ROM_REGION_INDEX)
		return -EINVAL;

	pgoff = vma->vm_pgoff &
		((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);

	vma->vm_pgoff = (pci_resource_start(pdev, index) >> PAGE_SHIFT) + pgoff;

	vma->vm_private_data = vdev;
	vma->vm_ops = &mdev_dev_mmio_ops;

	return 0;
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
	struct vfio_mdevice *vdev;
	struct mdev_device *mdevice = to_mdev_device(dev);
	int ret = 0;

	if (mdevice == NULL)
		return -EINVAL;

	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
	if (IS_ERR(vdev))
		return PTR_ERR(vdev);

	vdev->mdevice = mdevice;
	vdev->group = mdevice->group;
	mutex_init(&vdev->vfio_mdev_lock);

	ret = vfio_add_group_dev(dev, &vfio_mpci_dev_ops, vdev);
	if (ret)
		kfree(vdev);

	return ret;
}

void vfio_mpci_remove(struct device *dev)
{
	struct vfio_mdevice *vdev;

	vdev = vfio_del_group_dev(dev);
	kfree(vdev);
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
