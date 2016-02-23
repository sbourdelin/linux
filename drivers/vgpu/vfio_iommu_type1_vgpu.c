/*
 * VGPU : IOMMU DMA mapping support for VGPU
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
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uuid.h>
#include <linux/vfio.h>
#include <linux/iommu.h>
#include <linux/vgpu.h>

#include "vgpu_private.h"

#define DRIVER_VERSION	"0.1"
#define DRIVER_AUTHOR	"NVIDIA Corporation"
#define DRIVER_DESC     "VGPU Type1 IOMMU driver for VFIO"

// VFIO structures

struct vfio_iommu_vgpu {
	struct mutex lock;
	struct iommu_group *group;
	struct vgpu_device *vgpu_dev;
	struct rb_root dma_list;
	struct mm_struct * vm_mm;
};

struct vgpu_vfio_dma {
	struct rb_node node;
	dma_addr_t iova;
	unsigned long vaddr;
	size_t size;
	int prot;
};

/*
 * VGPU VFIO FOPs definition
 *
 */

/*
 * Duplicated from vfio_link_dma, just quick hack ... should
 * reuse code later
 */

static void vgpu_link_dma(struct vfio_iommu_vgpu *iommu,
			  struct vgpu_vfio_dma *new)
{
	struct rb_node **link = &iommu->dma_list.rb_node, *parent = NULL;
	struct vgpu_vfio_dma *dma;

	while (*link) {
		parent = *link;
		dma = rb_entry(parent, struct vgpu_vfio_dma, node);

		if (new->iova + new->size <= dma->iova)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, &iommu->dma_list);
}

static struct vgpu_vfio_dma *vgpu_find_dma(struct vfio_iommu_vgpu *iommu,
					   dma_addr_t start, size_t size)
{
	struct rb_node *node = iommu->dma_list.rb_node;

	while (node) {
		struct vgpu_vfio_dma *dma = rb_entry(node, struct vgpu_vfio_dma, node);

		if (start + size <= dma->iova)
			node = node->rb_left;
		else if (start >= dma->iova + dma->size)
			node = node->rb_right;
		else
			return dma;
	}

	return NULL;
}

static void vgpu_unlink_dma(struct vfio_iommu_vgpu *iommu, struct vgpu_vfio_dma *old)
{
	rb_erase(&old->node, &iommu->dma_list);
}

static void vgpu_dump_dma(struct vfio_iommu_vgpu *iommu)
{
	struct vgpu_vfio_dma *c, *n;
	uint32_t i = 0;

	rbtree_postorder_for_each_entry_safe(c, n, &iommu->dma_list, node)
		printk(KERN_INFO "%s: dma[%d] iova:0x%llx, vaddr:0x%lx, size:0x%lx\n",
		       __FUNCTION__, i++, c->iova, c->vaddr, c->size);
}

static int vgpu_dma_do_track(struct vfio_iommu_vgpu * vgpu_iommu,
	struct vfio_iommu_type1_dma_map *map)
{
	dma_addr_t iova = map->iova;
	unsigned long vaddr = map->vaddr;
	int ret = 0, prot = 0;
	struct vgpu_vfio_dma *vgpu_dma;

	mutex_lock(&vgpu_iommu->lock);

	if (vgpu_find_dma(vgpu_iommu, map->iova, map->size)) {
		mutex_unlock(&vgpu_iommu->lock);
		return -EEXIST;
	}

	vgpu_dma = kzalloc(sizeof(*vgpu_dma), GFP_KERNEL);

	if (!vgpu_dma) {
		mutex_unlock(&vgpu_iommu->lock);
		return -ENOMEM;
	}

	vgpu_dma->iova = iova;
	vgpu_dma->vaddr = vaddr;
	vgpu_dma->prot = prot;
	vgpu_dma->size = map->size;

	vgpu_link_dma(vgpu_iommu, vgpu_dma);

	mutex_unlock(&vgpu_iommu->lock);
	return ret;
}

static int vgpu_dma_do_untrack(struct vfio_iommu_vgpu * vgpu_iommu,
	struct vfio_iommu_type1_dma_unmap *unmap)
{
	struct vgpu_vfio_dma *vgpu_dma;
	size_t unmapped = 0;
	int ret = 0;

	mutex_lock(&vgpu_iommu->lock);

	vgpu_dma = vgpu_find_dma(vgpu_iommu, unmap->iova, 0);
	if (vgpu_dma && vgpu_dma->iova != unmap->iova) {
		ret = -EINVAL;
		goto unlock;
	}

	vgpu_dma = vgpu_find_dma(vgpu_iommu, unmap->iova + unmap->size - 1, 0);
	if (vgpu_dma && vgpu_dma->iova + vgpu_dma->size != unmap->iova + unmap->size) {
		ret = -EINVAL;
		goto unlock;
	}

	while (( vgpu_dma = vgpu_find_dma(vgpu_iommu, unmap->iova, unmap->size))) {
		unmapped += vgpu_dma->size;
		vgpu_unlink_dma(vgpu_iommu, vgpu_dma);
	}

unlock:
	mutex_unlock(&vgpu_iommu->lock);
	unmap->size = unmapped;

	return ret;
}

/* Ugly hack to quickly test single deivce ... */

static struct vfio_iommu_vgpu *_local_iommu = NULL;

int vgpu_dma_do_translate(dma_addr_t *gfn_buffer, uint32_t count)
{
	int i = 0, ret = 0, prot = 0;
	unsigned long remote_vaddr = 0, pfn = 0;
	struct vfio_iommu_vgpu *vgpu_iommu = _local_iommu;
	struct vgpu_vfio_dma *vgpu_dma;
	struct page *page[1];
	// unsigned long * addr = NULL;
	struct mm_struct *mm = vgpu_iommu->vm_mm;

	prot = IOMMU_READ | IOMMU_WRITE;

	printk(KERN_INFO "%s: >>>>\n", __FUNCTION__);

	mutex_lock(&vgpu_iommu->lock);

	vgpu_dump_dma(vgpu_iommu);

	for (i = 0; i < count; i++) {
		dma_addr_t iova = gfn_buffer[i] << PAGE_SHIFT;
		vgpu_dma = vgpu_find_dma(vgpu_iommu, iova, 0 /*  size */);

		if (!vgpu_dma) {
			printk(KERN_INFO "%s: fail locate iova[%d]:0x%llx\n", __FUNCTION__, i, iova);
			ret = -EINVAL;
			goto unlock;
		}

		remote_vaddr = vgpu_dma->vaddr + iova - vgpu_dma->iova;
		printk(KERN_INFO "%s: find dma iova[%d]:0x%llx, vaddr:0x%lx, size:0x%lx, remote_vaddr:0x%lx\n",
			__FUNCTION__, i, vgpu_dma->iova,
			vgpu_dma->vaddr, vgpu_dma->size, remote_vaddr);

		if (get_user_pages_unlocked(NULL, mm, remote_vaddr, 1, 1, 0, page) == 1) {
			pfn = page_to_pfn(page[0]);
			printk(KERN_INFO "%s: pfn[%d]:0x%lx\n", __FUNCTION__, i, pfn);
			// addr = vmap(page, 1, VM_MAP, PAGE_KERNEL);
		}
		else {
			printk(KERN_INFO "%s: fail to pin pfn[%d]\n", __FUNCTION__, i);
			ret = -ENOMEM;
			goto unlock;
		}

		gfn_buffer[i] = pfn;
		// vunmap(addr);

	}

unlock:
	mutex_unlock(&vgpu_iommu->lock);
	printk(KERN_INFO "%s: <<<<\n", __FUNCTION__);
	return ret;
}
EXPORT_SYMBOL(vgpu_dma_do_translate);

static void *vfio_iommu_vgpu_open(unsigned long arg)
{
	struct vfio_iommu_vgpu *iommu;

	iommu = kzalloc(sizeof(*iommu), GFP_KERNEL);

	if (!iommu)
		return ERR_PTR(-ENOMEM);

	mutex_init(&iommu->lock);

	printk(KERN_INFO "%s", __FUNCTION__);

	/* TODO: Keep track the v2 vs. v1, for now only assume
	 * we are v2 due to QEMU code */
	_local_iommu = iommu;
	return iommu;
}

static void vfio_iommu_vgpu_release(void *iommu_data)
{
	struct vfio_iommu_vgpu *iommu = iommu_data;
	kfree(iommu);
	printk(KERN_INFO "%s", __FUNCTION__);
}

static long vfio_iommu_vgpu_ioctl(void *iommu_data,
		unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	unsigned long minsz;
	struct vfio_iommu_vgpu *vgpu_iommu = iommu_data;

	switch (cmd) {
	case VFIO_CHECK_EXTENSION:
	{
		if ((arg == VFIO_TYPE1_IOMMU) || (arg == VFIO_TYPE1v2_IOMMU))
			return 1;
		else
			return 0;
	}

	case VFIO_IOMMU_GET_INFO:
	{
		struct vfio_iommu_type1_info info;
		minsz = offsetofend(struct vfio_iommu_type1_info, iova_pgsizes);

		if (copy_from_user(&info, (void __user *)arg, minsz))
			return -EFAULT;

		if (info.argsz < minsz)
			return -EINVAL;

		info.flags = 0;

		return copy_to_user((void __user *)arg, &info, minsz);
	}
	case VFIO_IOMMU_MAP_DMA:
	{
		// TODO
		struct vfio_iommu_type1_dma_map map;
		minsz = offsetofend(struct vfio_iommu_type1_dma_map, size);

		if (copy_from_user(&map, (void __user *)arg, minsz))
			return -EFAULT;

		if (map.argsz < minsz)
			return -EINVAL;

		printk(KERN_INFO "VGPU-IOMMU:MAP_DMA flags:%d, vaddr:0x%llx, iova:0x%llx, size:0x%llx\n",
			map.flags, map.vaddr, map.iova, map.size);

		/*
		 * TODO: Tracking code is mostly duplicated from TYPE1 IOMMU, ideally,
		 * this should be merged into one single file and reuse data
		 * structure
		 *
		 */
		ret = vgpu_dma_do_track(vgpu_iommu, &map);
		break;
	}
	case VFIO_IOMMU_UNMAP_DMA:
	{
		// TODO
		struct vfio_iommu_type1_dma_unmap unmap;

		minsz = offsetofend(struct vfio_iommu_type1_dma_unmap, size);

		if (copy_from_user(&unmap, (void __user *)arg, minsz))
			return -EFAULT;

		if (unmap.argsz < minsz)
			return -EINVAL;

		ret = vgpu_dma_do_untrack(vgpu_iommu, &unmap);
		break;
	}
	default:
	{
		printk(KERN_INFO "%s cmd default ", __FUNCTION__);
		ret = -ENOTTY;
		break;
	}
	}

	return ret;
}


static int vfio_iommu_vgpu_attach_group(void *iommu_data,
		                        struct iommu_group *iommu_group)
{
	struct vfio_iommu_vgpu *iommu = iommu_data;
	struct vgpu_device *vgpu_dev = NULL;

	printk(KERN_INFO "%s", __FUNCTION__);

	vgpu_dev = get_vgpu_device_from_group(iommu_group);
	if (vgpu_dev) {
		iommu->vgpu_dev = vgpu_dev;
		iommu->group = iommu_group;

		/* IOMMU shares the same life cylce as VM MM */
		iommu->vm_mm = current->mm;

		return 0;
	}
	iommu->group = iommu_group;
	return 1;
}

static void vfio_iommu_vgpu_detach_group(void *iommu_data,
		struct iommu_group *iommu_group)
{
	struct vfio_iommu_vgpu *iommu = iommu_data;

	printk(KERN_INFO "%s", __FUNCTION__);
	iommu->vm_mm = NULL;
	iommu->group = NULL;

	return;
}


static const struct vfio_iommu_driver_ops vfio_iommu_vgpu_driver_ops = {
	.name           = "vgpu_vfio",
	.owner          = THIS_MODULE,
	.open           = vfio_iommu_vgpu_open,
	.release        = vfio_iommu_vgpu_release,
	.ioctl          = vfio_iommu_vgpu_ioctl,
	.attach_group   = vfio_iommu_vgpu_attach_group,
	.detach_group   = vfio_iommu_vgpu_detach_group,
};


int vgpu_vfio_iommu_init(void)
{
	int rc = vfio_register_iommu_driver(&vfio_iommu_vgpu_driver_ops);

	printk(KERN_INFO "%s\n", __FUNCTION__);
	if (rc < 0) {
		printk(KERN_ERR "Error: failed to register vfio iommu, err:%d\n", rc);
	}

	return rc;
}

void vgpu_vfio_iommu_exit(void)
{
	// unregister vgpu_vfio driver
	vfio_unregister_iommu_driver(&vfio_iommu_vgpu_driver_ops);
	printk(KERN_INFO "%s\n", __FUNCTION__);
}


module_init(vgpu_vfio_iommu_init);
module_exit(vgpu_vfio_iommu_exit);

MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);

