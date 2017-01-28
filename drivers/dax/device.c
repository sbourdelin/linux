/*
 * Copyright(c) 2016 - 2017 Intel Corporation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/pagemap.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/pfn_t.h>
#include <linux/slab.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include "dax.h"

static struct class *dax_class;

/**
 * struct dax_region - mapping infrastructure for dax devices
 * @id: kernel-wide unique region for a memory range
 * @base: linear address corresponding to @res
 * @kref: to pin while other agents have a need to do lookups
 * @dev: parent device backing this region
 * @align: allocation and mapping alignment for child dax devices
 * @res: physical address range of the region
 * @pfn_flags: identify whether the pfns are paged back or not
 */
struct dax_region {
	int id;
	struct ida ida;
	void *base;
	struct kref kref;
	struct device *dev;
	unsigned int align;
	struct resource res;
	unsigned long pfn_flags;
};

/**
 * struct dax_dev - subdivision of a dax region
 * @region - parent region
 * @dax_inode - core dax functionality
 * @dev - device core
 * @id - child id in the region
 * @num_resources - number of physical address extents in this device
 * @res - array of physical address ranges
 */
struct dax_dev {
	struct dax_region *region;
	struct dax_inode *dax_inode;
	struct device dev;
	int id;
	int num_resources;
	struct resource res[0];
};

static ssize_t id_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dax_region *dax_region;
	ssize_t rc = -ENXIO;

	device_lock(dev);
	dax_region = dev_get_drvdata(dev);
	if (dax_region)
		rc = sprintf(buf, "%d\n", dax_region->id);
	device_unlock(dev);

	return rc;
}
static DEVICE_ATTR_RO(id);

static ssize_t region_size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dax_region *dax_region;
	ssize_t rc = -ENXIO;

	device_lock(dev);
	dax_region = dev_get_drvdata(dev);
	if (dax_region)
		rc = sprintf(buf, "%llu\n", (unsigned long long)
				resource_size(&dax_region->res));
	device_unlock(dev);

	return rc;
}
static struct device_attribute dev_attr_region_size = __ATTR(size, 0444,
		region_size_show, NULL);

static ssize_t align_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dax_region *dax_region;
	ssize_t rc = -ENXIO;

	device_lock(dev);
	dax_region = dev_get_drvdata(dev);
	if (dax_region)
		rc = sprintf(buf, "%u\n", dax_region->align);
	device_unlock(dev);

	return rc;
}
static DEVICE_ATTR_RO(align);

static struct attribute *dax_region_attributes[] = {
	&dev_attr_region_size.attr,
	&dev_attr_align.attr,
	&dev_attr_id.attr,
	NULL,
};

static const struct attribute_group dax_region_attribute_group = {
	.name = "dax_region",
	.attrs = dax_region_attributes,
};

static const struct attribute_group *dax_region_attribute_groups[] = {
	&dax_region_attribute_group,
	NULL,
};

static void dax_region_free(struct kref *kref)
{
	struct dax_region *dax_region;

	dax_region = container_of(kref, struct dax_region, kref);
	kfree(dax_region);
}

void dax_region_put(struct dax_region *dax_region)
{
	kref_put(&dax_region->kref, dax_region_free);
}
EXPORT_SYMBOL_GPL(dax_region_put);

static void dax_region_unregister(void *region)
{
	struct dax_region *dax_region = region;

	sysfs_remove_groups(&dax_region->dev->kobj,
			dax_region_attribute_groups);
	dax_region_put(dax_region);
}

struct dax_region *alloc_dax_region(struct device *parent, int region_id,
		struct resource *res, unsigned int align, void *addr,
		unsigned long pfn_flags)
{
	struct dax_region *dax_region;

	/*
	 * The DAX core assumes that it can store its private data in
	 * parent->driver_data. This WARN is a reminder / safeguard for
	 * developers of device-dax drivers.
	 */
	if (dev_get_drvdata(parent)) {
		dev_WARN(parent, "dax core failed to setup private data\n");
		return NULL;
	}

	if (!IS_ALIGNED(res->start, align)
			|| !IS_ALIGNED(resource_size(res), align))
		return NULL;

	dax_region = kzalloc(sizeof(*dax_region), GFP_KERNEL);
	if (!dax_region)
		return NULL;

	dev_set_drvdata(parent, dax_region);
	memcpy(&dax_region->res, res, sizeof(*res));
	dax_region->pfn_flags = pfn_flags;
	kref_init(&dax_region->kref);
	dax_region->id = region_id;
	ida_init(&dax_region->ida);
	dax_region->align = align;
	dax_region->dev = parent;
	dax_region->base = addr;
	if (sysfs_create_groups(&parent->kobj, dax_region_attribute_groups)) {
		kfree(dax_region);
		return NULL;;
	}

	kref_get(&dax_region->kref);
	if (devm_add_action_or_reset(parent, dax_region_unregister, dax_region))
		return NULL;
	return dax_region;
}
EXPORT_SYMBOL_GPL(alloc_dax_region);

static struct dax_dev *to_dax_dev(struct device *dev)
{
	return container_of(dev, struct dax_dev, dev);
}

static ssize_t size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct dax_dev *dax_dev = to_dax_dev(dev);
	unsigned long long size = 0;
	int i;

	for (i = 0; i < dax_dev->num_resources; i++)
		size += resource_size(&dax_dev->res[i]);

	return sprintf(buf, "%llu\n", size);
}
static DEVICE_ATTR_RO(size);

static struct attribute *dax_device_attributes[] = {
	&dev_attr_size.attr,
	NULL,
};

static const struct attribute_group dax_device_attribute_group = {
	.attrs = dax_device_attributes,
};

static const struct attribute_group *dax_attribute_groups[] = {
	&dax_device_attribute_group,
	NULL,
};

static int check_vma(struct dax_dev *dax_dev, struct vm_area_struct *vma,
		const char *func)
{
	struct dax_region *dax_region = dax_dev->region;
	struct device *dev = &dax_dev->dev;
	unsigned long mask;

	if (!dax_inode_alive(dax_dev->dax_inode))
		return -ENXIO;

	/* prevent private mappings from being established */
	if ((vma->vm_flags & VM_MAYSHARE) != VM_MAYSHARE) {
		dev_info(dev, "%s: %s: fail, attempted private mapping\n",
				current->comm, func);
		return -EINVAL;
	}

	mask = dax_region->align - 1;
	if (vma->vm_start & mask || vma->vm_end & mask) {
		dev_info(dev, "%s: %s: fail, unaligned vma (%#lx - %#lx, %#lx)\n",
				current->comm, func, vma->vm_start, vma->vm_end,
				mask);
		return -EINVAL;
	}

	if ((dax_region->pfn_flags & (PFN_DEV|PFN_MAP)) == PFN_DEV
			&& (vma->vm_flags & VM_DONTCOPY) == 0) {
		dev_info(dev, "%s: %s: fail, dax range requires MADV_DONTFORK\n",
				current->comm, func);
		return -EINVAL;
	}

	if (!vma_is_dax(vma)) {
		dev_info(dev, "%s: %s: fail, vma is not DAX capable\n",
				current->comm, func);
		return -EINVAL;
	}

	return 0;
}

static phys_addr_t pgoff_to_phys(struct dax_dev *dax_dev, pgoff_t pgoff,
		unsigned long size)
{
	struct resource *res;
	phys_addr_t phys;
	int i;

	for (i = 0; i < dax_dev->num_resources; i++) {
		res = &dax_dev->res[i];
		phys = pgoff * PAGE_SIZE + res->start;
		if (phys >= res->start && phys <= res->end)
			break;
		pgoff -= PHYS_PFN(resource_size(res));
	}

	if (i < dax_dev->num_resources) {
		res = &dax_dev->res[i];
		if (phys + size - 1 <= res->end)
			return phys;
	}

	return -1;
}

static int __dax_dev_fault(struct dax_dev *dax_dev, struct vm_area_struct *vma,
		struct vm_fault *vmf)
{
	struct device *dev = &dax_dev->dev;
	struct dax_region *dax_region;
	int rc = VM_FAULT_SIGBUS;
	phys_addr_t phys;
	pfn_t pfn;

	if (check_vma(dax_dev, vma, __func__))
		return VM_FAULT_SIGBUS;

	dax_region = dax_dev->region;
	if (dax_region->align > PAGE_SIZE) {
		dev_dbg(dev, "%s: alignment > fault size\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	phys = pgoff_to_phys(dax_dev, vmf->pgoff, PAGE_SIZE);
	if (phys == -1) {
		dev_dbg(dev, "%s: phys_to_pgoff(%#lx) failed\n", __func__,
				vmf->pgoff);
		return VM_FAULT_SIGBUS;
	}

	pfn = phys_to_pfn_t(phys, dax_region->pfn_flags);

	rc = vm_insert_mixed(vma, vmf->address, pfn);

	if (rc == -ENOMEM)
		return VM_FAULT_OOM;
	if (rc < 0 && rc != -EBUSY)
		return VM_FAULT_SIGBUS;

	return VM_FAULT_NOPAGE;
}

static int dax_dev_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	int rc, id;
	struct file *filp = vma->vm_file;
	struct dax_dev *dax_dev = filp->private_data;

	dev_dbg(&dax_dev->dev, "%s: %s: %s (%#lx - %#lx)\n", __func__,
			current->comm, (vmf->flags & FAULT_FLAG_WRITE)
			? "write" : "read", vma->vm_start, vma->vm_end);
	id = dax_read_lock();
	rc = __dax_dev_fault(dax_dev, vma, vmf);
	dax_read_unlock(id);

	return rc;
}

static int __dax_dev_pmd_fault(struct dax_dev *dax_dev,
		struct vm_area_struct *vma, unsigned long addr, pmd_t *pmd,
		unsigned int flags)
{
	unsigned long pmd_addr = addr & PMD_MASK;
	struct device *dev = &dax_dev->dev;
	struct dax_region *dax_region;
	phys_addr_t phys;
	pgoff_t pgoff;
	pfn_t pfn;

	if (check_vma(dax_dev, vma, __func__))
		return VM_FAULT_SIGBUS;

	dax_region = dax_dev->region;
	if (dax_region->align > PMD_SIZE) {
		dev_dbg(dev, "%s: alignment > fault size\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	/* dax pmd mappings require pfn_t_devmap() */
	if ((dax_region->pfn_flags & (PFN_DEV|PFN_MAP)) != (PFN_DEV|PFN_MAP)) {
		dev_dbg(dev, "%s: alignment > fault size\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	pgoff = linear_page_index(vma, pmd_addr);
	phys = pgoff_to_phys(dax_dev, pgoff, PMD_SIZE);
	if (phys == -1) {
		dev_dbg(dev, "%s: phys_to_pgoff(%#lx) failed\n", __func__,
				pgoff);
		return VM_FAULT_SIGBUS;
	}

	pfn = phys_to_pfn_t(phys, dax_region->pfn_flags);

	return vmf_insert_pfn_pmd(vma, addr, pmd, pfn,
			flags & FAULT_FLAG_WRITE);
}

static int dax_dev_pmd_fault(struct vm_area_struct *vma, unsigned long addr,
		pmd_t *pmd, unsigned int flags)
{
	int rc, id;
	struct file *filp = vma->vm_file;
	struct dax_dev *dax_dev = filp->private_data;

	dev_dbg(&dax_dev->dev, "%s: %s: %s (%#lx - %#lx)\n", __func__,
			current->comm, (flags & FAULT_FLAG_WRITE)
			? "write" : "read", vma->vm_start, vma->vm_end);

	id = dax_read_lock();
	rc = __dax_dev_pmd_fault(dax_dev, vma, addr, pmd, flags);
	dax_read_unlock(id);

	return rc;
}

static const struct vm_operations_struct dax_dev_vm_ops = {
	.fault = dax_dev_fault,
	.pmd_fault = dax_dev_pmd_fault,
};

static int dax_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int rc, id;
	struct dax_dev *dax_dev = filp->private_data;

	dev_dbg(&dax_dev->dev, "%s\n", __func__);

	/*
	 * We lock to check dax_inode liveness and will re-check at
	 * fault time.
	 */
	id = dax_read_lock();
	rc = check_vma(dax_dev, vma, __func__);
	dax_read_unlock(id);
	if (rc)
		return rc;

	vma->vm_ops = &dax_dev_vm_ops;
	vma->vm_flags |= VM_MIXEDMAP | VM_HUGEPAGE;
	return 0;
}

/* return an unmapped area aligned to the dax region specified alignment */
static unsigned long dax_get_unmapped_area(struct file *filp,
		unsigned long addr, unsigned long len, unsigned long pgoff,
		unsigned long flags)
{
	unsigned long off, off_end, off_align, len_align, addr_align, align;
	struct dax_dev *dax_dev = filp ? filp->private_data : NULL;
	struct dax_region *dax_region;

	if (!dax_dev || addr)
		goto out;

	dax_region = dax_dev->region;
	align = dax_region->align;
	off = pgoff << PAGE_SHIFT;
	off_end = off + len;
	off_align = round_up(off, align);

	if ((off_end <= off_align) || ((off_end - off_align) < align))
		goto out;

	len_align = len + align;
	if ((off + len_align) < off)
		goto out;

	addr_align = current->mm->get_unmapped_area(filp, addr, len_align,
			pgoff, flags);
	if (!IS_ERR_VALUE(addr_align)) {
		addr_align += (off - addr_align) & (align - 1);
		return addr_align;
	}
 out:
	return current->mm->get_unmapped_area(filp, addr, len, pgoff, flags);
}

static int dax_open(struct inode *inode, struct file *filp)
{
	struct dax_inode *dax_inode = inode_to_dax_inode(inode);
	struct inode *__dax_inode = dax_inode_to_inode(dax_inode);
	struct dax_dev *dax_dev = dax_inode_get_private(dax_inode);

	dev_dbg(&dax_dev->dev, "%s\n", __func__);
	inode->i_mapping = __dax_inode->i_mapping;
	inode->i_mapping->host = __dax_inode;
	filp->f_mapping = inode->i_mapping;
	filp->private_data = dax_dev;
	inode->i_flags = S_DAX;

	return 0;
}

static int dax_release(struct inode *inode, struct file *filp)
{
	struct dax_dev *dax_dev = filp->private_data;

	dev_dbg(&dax_dev->dev, "%s\n", __func__);
	return 0;
}

static const struct file_operations dax_fops = {
	.llseek = noop_llseek,
	.owner = THIS_MODULE,
	.open = dax_open,
	.release = dax_release,
	.get_unmapped_area = dax_get_unmapped_area,
	.mmap = dax_mmap,
};

static void dax_dev_release(struct device *dev)
{
	struct dax_dev *dax_dev = to_dax_dev(dev);
	struct dax_region *dax_region = dax_dev->region;
	struct dax_inode *dax_inode = dax_dev->dax_inode;

	ida_simple_remove(&dax_region->ida, dax_dev->id);
	dax_region_put(dax_region);
	put_dax_inode(dax_inode);
	kfree(dax_dev);
}

static void unregister_dax_dev(void *dev)
{
	struct dax_dev *dax_dev = to_dax_dev(dev);
	struct dax_inode *dax_inode = dax_dev->dax_inode;
	struct inode *inode = dax_inode_to_inode(dax_inode);

	dev_dbg(dev, "%s\n", __func__);

	kill_dax_inode(dax_inode);
	unmap_mapping_range(inode->i_mapping, 0, 0, 1);
	dax_inode_unregister(dax_inode);
	device_unregister(dev);
}

struct dax_dev *devm_create_dax_dev(struct dax_region *dax_region,
		struct resource *res, int count)
{
	struct device *parent = dax_region->dev;
	struct dax_inode *dax_inode;
	struct dax_dev *dax_dev;
	struct inode *inode;
	struct device *dev;
	int rc = 0, i;

	dax_dev = kzalloc(sizeof(*dax_dev) + sizeof(*res) * count, GFP_KERNEL);
	if (!dax_dev)
		return ERR_PTR(-ENOMEM);

	for (i = 0; i < count; i++) {
		if (!IS_ALIGNED(res[i].start, dax_region->align)
				|| !IS_ALIGNED(resource_size(&res[i]),
					dax_region->align)) {
			rc = -EINVAL;
			break;
		}
		dax_dev->res[i].start = res[i].start;
		dax_dev->res[i].end = res[i].end;
	}

	if (i < count)
		goto err_id;

	dax_dev->id = ida_simple_get(&dax_region->ida, 0, 0, GFP_KERNEL);
	if (dax_dev->id < 0) {
		rc = dax_dev->id;
		goto err_id;
	}

	dax_inode = alloc_dax_inode(dax_dev, NULL);
	if (!dax_inode)
		goto err_inode;

	/* initialize now so dax_inode_register() can reference dev->kobj */
	dax_dev->dax_inode = dax_inode;
	dev = &dax_dev->dev;
	device_initialize(dev);

	rc = dax_inode_register(dax_inode, &dax_fops,
			parent->driver->owner, &dev->kobj);
	if (rc)
		goto err_register;

	/* from here on we're committed to teardown via dax_dev_release() */
	dax_dev->num_resources = count;
	dax_dev->region = dax_region;
	kref_get(&dax_region->kref);

	inode = dax_inode_to_inode(dax_inode);
	dev->devt = inode->i_rdev;
	dev->class = dax_class;
	dev->parent = parent;
	dev->groups = dax_attribute_groups;
	dev->release = dax_dev_release;
	dev_set_name(dev, "dax%d.%d", dax_region->id, dax_dev->id);
	rc = device_add(dev);
	if (rc) {
		put_device(dev);
		return ERR_PTR(rc);
	}

	rc = devm_add_action_or_reset(dax_region->dev, unregister_dax_dev, dev);
	if (rc)
		return ERR_PTR(rc);

	return dax_dev;

 err_register:
	put_dax_inode(dax_inode);
 err_inode:
	ida_simple_remove(&dax_region->ida, dax_dev->id);
 err_id:
	kfree(dax_dev);

	return ERR_PTR(rc);
}
EXPORT_SYMBOL_GPL(devm_create_dax_dev);

static int __init dax_init(void)
{
	dax_class = class_create(THIS_MODULE, "dax");
	return PTR_ERR_OR_ZERO(dax_class);
}

static void __exit dax_exit(void)
{
	class_destroy(dax_class);
}

MODULE_AUTHOR("Intel Corporation");
MODULE_LICENSE("GPL v2");
subsys_initcall(dax_init);
module_exit(dax_exit);
