/*
 * VGPU VFIO device
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
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uuid.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/vgpu.h>

#include "vgpu_private.h"

#define DRIVER_VERSION  "0.1"
#define DRIVER_AUTHOR   "NVIDIA Corporation"
#define DRIVER_DESC     "VGPU VFIO Driver"

#define VFIO_PCI_OFFSET_SHIFT   40

#define VFIO_PCI_OFFSET_TO_INDEX(off)	(off >> VFIO_PCI_OFFSET_SHIFT)
#define VFIO_PCI_INDEX_TO_OFFSET(index)	((u64)(index) << VFIO_PCI_OFFSET_SHIFT)
#define VFIO_PCI_OFFSET_MASK	(((u64)(1) << VFIO_PCI_OFFSET_SHIFT) - 1)

struct vfio_vgpu_device {
	struct iommu_group *group;
	struct vgpu_device *vgpu_dev;
	int		    refcnt;
	struct pci_bar_info bar_info[VFIO_PCI_NUM_REGIONS];
	u8		    *vconfig;
};

static DEFINE_MUTEX(vfio_vgpu_lock);

static int get_virtual_bar_info(struct vgpu_device *vgpu_dev,
				struct pci_bar_info *bar_info,
				int index)
{
	int ret = -1;
	struct gpu_device *gpu_dev = vgpu_dev->gpu_dev;

	if (gpu_dev->ops->vgpu_bar_info)
		ret = gpu_dev->ops->vgpu_bar_info(vgpu_dev, index, bar_info);
	return ret;
}

static int vdev_read_base(struct vfio_vgpu_device *vdev)
{
	int index, pos;
	u32 start_lo, start_hi;
	u32 mem_type;

	pos = PCI_BASE_ADDRESS_0;

	for (index = 0; index <= VFIO_PCI_BAR5_REGION_INDEX; index++) {

		if (!vdev->bar_info[index].size)
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
		vdev->bar_info[index].start = ((u64)start_hi << 32) | start_lo;
	}
	return 0;
}

static int vgpu_dev_open(void *device_data)
{
	int ret = 0;
	struct vfio_vgpu_device *vdev = device_data;

	if (!try_module_get(THIS_MODULE))
		return -ENODEV;

	mutex_lock(&vfio_vgpu_lock);

	if (!vdev->refcnt) {
		u8 *vconfig;
		int vconfig_size, index;

		for (index = 0; index < VFIO_PCI_NUM_REGIONS; index++) {
			ret = get_virtual_bar_info(vdev->vgpu_dev,
						   &vdev->bar_info[index],
						   index);
			if (ret)
				goto open_error;
		}
		vconfig_size = vdev->bar_info[VFIO_PCI_CONFIG_REGION_INDEX].size;
		if (!vconfig_size)
			goto open_error;

		vconfig = kzalloc(vconfig_size, GFP_KERNEL);
		if (!vconfig) {
			ret = -ENOMEM;
			goto open_error;
		}

		vdev->vconfig = vconfig;
	}

	vdev->refcnt++;
open_error:

	mutex_unlock(&vfio_vgpu_lock);

	if (ret)
		module_put(THIS_MODULE);

	return ret;
}

static void vgpu_dev_close(void *device_data)
{
	struct vfio_vgpu_device *vdev = device_data;

	mutex_lock(&vfio_vgpu_lock);

	vdev->refcnt--;
	if (!vdev->refcnt) {
		memset(&vdev->bar_info, 0, sizeof(vdev->bar_info));
		if (vdev->vconfig)
			kfree(vdev->vconfig);
	}

	mutex_unlock(&vfio_vgpu_lock);

	module_put(THIS_MODULE);
}

static int vgpu_get_irq_count(struct vfio_vgpu_device *vdev, int irq_type)
{
	// Don't support MSIX for now
	if (irq_type == VFIO_PCI_MSIX_IRQ_INDEX)
		return -1;

	return 1;
}

static long vgpu_dev_unlocked_ioctl(void *device_data,
		unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	struct vfio_vgpu_device *vdev = device_data;
	unsigned long minsz;

	switch (cmd)
	{
	case VFIO_DEVICE_GET_INFO:
	{
		struct vfio_device_info info;
		printk(KERN_INFO "%s VFIO_DEVICE_GET_INFO cmd index ", __FUNCTION__);
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

		printk(KERN_INFO "%s VFIO_DEVICE_GET_REGION_INFO cmd for region_index %d", __FUNCTION__, info.index);
		switch (info.index) {
		case VFIO_PCI_CONFIG_REGION_INDEX:
		case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = vdev->bar_info[info.index].size;
			if (!info.size) {
				info.flags = 0;
				break;
			}

			info.flags = vdev->bar_info[info.index].flags;
			break;
		case VFIO_PCI_VGA_REGION_INDEX:
			info.offset = VFIO_PCI_INDEX_TO_OFFSET(info.index);
			info.size = 0xc0000;
			info.flags = VFIO_REGION_INFO_FLAG_READ |
				     VFIO_REGION_INFO_FLAG_WRITE;
				break;

		case VFIO_PCI_ROM_REGION_INDEX:
		default:
			return -EINVAL;
		}

		return copy_to_user((void __user *)arg, &info, minsz);

	}
	case VFIO_DEVICE_GET_IRQ_INFO:
	{
		struct vfio_irq_info info;

		printk(KERN_INFO "%s VFIO_DEVICE_GET_IRQ_INFO cmd", __FUNCTION__);
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
		info.count = vgpu_get_irq_count(vdev, info.index);

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
		struct gpu_device *gpu_dev = vdev->vgpu_dev->gpu_dev;
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
			int max = vgpu_get_irq_count(vdev, hdr.index);

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

			if (gpu_dev->ops->vgpu_set_irqs) {
				ret = gpu_dev->ops->vgpu_set_irqs(vdev->vgpu_dev,
								  hdr.flags,
								  hdr.index, hdr.start,
								  hdr.count, data);
			}
			kfree(data);
			return ret;
		}

		default:
			return -EINVAL;
	}
	return ret;
}

ssize_t vgpu_dev_config_rw(struct vfio_vgpu_device *vdev, char __user *buf,
		size_t count, loff_t *ppos, bool iswrite)
{
	struct vgpu_device *vgpu_dev = vdev->vgpu_dev;
	struct gpu_device *gpu_dev = vgpu_dev->gpu_dev;
	int cfg_size = vdev->bar_info[VFIO_PCI_CONFIG_REGION_INDEX].size;
	int ret = 0;
	uint64_t pos = *ppos & VFIO_PCI_OFFSET_MASK;

	if (pos < 0 || pos >= cfg_size ||
	    pos + count > cfg_size) {
		printk(KERN_ERR "%s pos 0x%llx out of range\n", __FUNCTION__, pos);
		ret = -EFAULT;
		goto config_rw_exit;
	}

	if (iswrite) {
		char *user_data = kmalloc(count, GFP_KERNEL);

		if (user_data == NULL) {
			ret = -ENOMEM;
			goto config_rw_exit;
		}

		if (copy_from_user(user_data, buf, count)) {
			ret = -EFAULT;
			kfree(user_data);
			goto config_rw_exit;
		}

		if (gpu_dev->ops->write) {
			ret = gpu_dev->ops->write(vgpu_dev,
						  user_data,
						  count,
						  vgpu_emul_space_config,
						  pos);
		}

		memcpy((void *)(vdev->vconfig + pos), (void *)user_data, count);
		kfree(user_data);
	}
	else
	{
		char *ret_data = kzalloc(count, GFP_KERNEL);

		if (ret_data == NULL) {
			ret = -ENOMEM;
			goto config_rw_exit;
		}

		if (gpu_dev->ops->read) {
			ret = gpu_dev->ops->read(vgpu_dev,
						 ret_data,
						 count,
						 vgpu_emul_space_config,
						 pos);
		}

		if (ret > 0 ) {
			if (copy_to_user(buf, ret_data, ret)) {
				ret = -EFAULT;
				kfree(ret_data);
				goto config_rw_exit;
			}

			memcpy((void *)(vdev->vconfig + pos), (void *)ret_data, count);
		}
		kfree(ret_data);
	}
config_rw_exit:
	return ret;
}

ssize_t vgpu_dev_bar_rw(struct vfio_vgpu_device *vdev, char __user *buf,
		size_t count, loff_t *ppos, bool iswrite)
{
	struct vgpu_device *vgpu_dev = vdev->vgpu_dev;
	struct gpu_device *gpu_dev = vgpu_dev->gpu_dev;
	loff_t offset = *ppos & VFIO_PCI_OFFSET_MASK;
	loff_t pos;
	int bar_index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	int ret = 0;

	if (!vdev->bar_info[bar_index].start) {
		ret = vdev_read_base(vdev);
		if (ret)
			goto bar_rw_exit;
	}

	if (offset >= vdev->bar_info[bar_index].size) {
		ret = -EINVAL;
		goto bar_rw_exit;
	}

	pos = vdev->bar_info[bar_index].start + offset;
	if (iswrite) {
		char *user_data = kmalloc(count, GFP_KERNEL);

		if (user_data == NULL) {
			ret = -ENOMEM;
			goto bar_rw_exit;
		}

		if (copy_from_user(user_data, buf, count)) {
			ret = -EFAULT;
			kfree(user_data);
			goto bar_rw_exit;
		}

		if (gpu_dev->ops->write) {
			ret = gpu_dev->ops->write(vgpu_dev,
						  user_data,
						  count,
						  vgpu_emul_space_mmio,
						  pos);
		}

		kfree(user_data);
	}
	else
	{
		char *ret_data = kmalloc(count, GFP_KERNEL);

		if (ret_data == NULL) {
			ret = -ENOMEM;
			goto bar_rw_exit;
		}

		memset(ret_data, 0, count);

		if (gpu_dev->ops->read) {
			ret = gpu_dev->ops->read(vgpu_dev,
						 ret_data,
						 count,
						 vgpu_emul_space_mmio,
						 pos);
		}

		if (ret > 0 ) {
			if (copy_to_user(buf, ret_data, ret)) {
				ret = -EFAULT;
			}
		}
		kfree(ret_data);
	}

bar_rw_exit:
	return ret;
}


static ssize_t vgpu_dev_rw(void *device_data, char __user *buf,
		size_t count, loff_t *ppos, bool iswrite)
{
	unsigned int index = VFIO_PCI_OFFSET_TO_INDEX(*ppos);
	struct vfio_vgpu_device *vdev = device_data;

	if (index >= VFIO_PCI_NUM_REGIONS)
		return -EINVAL;

	switch (index) {
	case VFIO_PCI_CONFIG_REGION_INDEX:
		return vgpu_dev_config_rw(vdev, buf, count, ppos, iswrite);

	case VFIO_PCI_BAR0_REGION_INDEX ... VFIO_PCI_BAR5_REGION_INDEX:
		return vgpu_dev_bar_rw(vdev, buf, count, ppos, iswrite);

	case VFIO_PCI_ROM_REGION_INDEX:
	case VFIO_PCI_VGA_REGION_INDEX:
		break;
	}

	return -EINVAL;
}


static ssize_t vgpu_dev_read(void *device_data, char __user *buf,
			     size_t count, loff_t *ppos)
{
	int ret = 0;

	if (count)
		ret = vgpu_dev_rw(device_data, buf, count, ppos, false);

	return ret;
}

static ssize_t vgpu_dev_write(void *device_data, const char __user *buf,
			      size_t count, loff_t *ppos)
{
	int ret = 0;

	if (count)
		ret = vgpu_dev_rw(device_data, (char *)buf, count, ppos, true);

	return ret;
}

static int vgpu_dev_mmio_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int ret = 0;
	struct vfio_vgpu_device *vdev = vma->vm_private_data;
	struct vgpu_device *vgpu_dev;
	struct gpu_device *gpu_dev;
	u64 virtaddr = (u64)vmf->virtual_address;
	u64 offset, phyaddr;
	unsigned long req_size, pgoff;
	pgprot_t pg_prot;

	if (!vdev && !vdev->vgpu_dev)
		return -EINVAL;

	vgpu_dev = vdev->vgpu_dev;
	gpu_dev  = vgpu_dev->gpu_dev;

	offset   = vma->vm_pgoff << PAGE_SHIFT;
	phyaddr  = virtaddr - vma->vm_start + offset;
	pgoff    = phyaddr >> PAGE_SHIFT;
	req_size = vma->vm_end - virtaddr;
	pg_prot  = vma->vm_page_prot;

	if (gpu_dev->ops->validate_map_request) {
		ret = gpu_dev->ops->validate_map_request(vgpu_dev, virtaddr, &pgoff,
							 &req_size, &pg_prot);
		if (ret)
			return ret;

		if (!req_size)
			return -EINVAL;
	}

	ret = remap_pfn_range(vma, virtaddr, pgoff, req_size, pg_prot);

	return ret | VM_FAULT_NOPAGE;
}

static const struct vm_operations_struct vgpu_dev_mmio_ops = {
	.fault = vgpu_dev_mmio_fault,
};


static int vgpu_dev_mmap(void *device_data, struct vm_area_struct *vma)
{
	unsigned int index;
	struct vfio_vgpu_device *vdev = device_data;
	struct vgpu_device *vgpu_dev = vdev->vgpu_dev;
	struct pci_dev *pdev = vgpu_dev->gpu_dev->dev;
	unsigned long pgoff;

	loff_t offset = vma->vm_pgoff << PAGE_SHIFT;

	index = VFIO_PCI_OFFSET_TO_INDEX(offset);

	if (index >= VFIO_PCI_ROM_REGION_INDEX)
		return -EINVAL;

	pgoff = vma->vm_pgoff &
		((1U << (VFIO_PCI_OFFSET_SHIFT - PAGE_SHIFT)) - 1);

	vma->vm_pgoff = (pci_resource_start(pdev, index) >> PAGE_SHIFT) + pgoff;

	vma->vm_private_data = vdev;
	vma->vm_ops = &vgpu_dev_mmio_ops;

	return 0;
}

static const struct vfio_device_ops vgpu_vfio_dev_ops = {
	.name		= "vfio-vgpu",
	.open		= vgpu_dev_open,
	.release	= vgpu_dev_close,
	.ioctl		= vgpu_dev_unlocked_ioctl,
	.read		= vgpu_dev_read,
	.write		= vgpu_dev_write,
	.mmap		= vgpu_dev_mmap,
};

int vgpu_vfio_probe(struct device *dev)
{
	struct vfio_vgpu_device *vdev;
	struct vgpu_device *vgpu_dev = to_vgpu_device(dev);
	int ret = 0;

	if (vgpu_dev == NULL)
		return -EINVAL;

	vdev = kzalloc(sizeof(*vdev), GFP_KERNEL);
	if (!vdev) {
		return -ENOMEM;
	}

	vdev->vgpu_dev = vgpu_dev;
	vdev->group = vgpu_dev->group;

	ret = vfio_add_group_dev(dev, &vgpu_vfio_dev_ops, vdev);
	if (ret)
		kfree(vdev);

	printk(KERN_INFO "%s ret = %d\n", __FUNCTION__, ret);
	return ret;
}

void vgpu_vfio_remove(struct device *dev)
{
	struct vfio_vgpu_device *vdev;

	printk(KERN_INFO "%s \n", __FUNCTION__);
	vdev = vfio_del_group_dev(dev);
	if (vdev) {
		printk(KERN_INFO "%s vdev being freed\n", __FUNCTION__);
		kfree(vdev);
	}
}

struct vgpu_driver vgpu_vfio_driver = {
        .name	= "vgpu-vfio",
        .probe	= vgpu_vfio_probe,
        .remove	= vgpu_vfio_remove,
};

static int __init vgpu_vfio_init(void)
{
	printk(KERN_INFO "%s \n", __FUNCTION__);
	return vgpu_register_driver(&vgpu_vfio_driver, THIS_MODULE);
}

static void __exit vgpu_vfio_exit(void)
{
	printk(KERN_INFO "%s \n", __FUNCTION__);
	vgpu_unregister_driver(&vgpu_vfio_driver);
}

module_init(vgpu_vfio_init)
module_exit(vgpu_vfio_exit)

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
