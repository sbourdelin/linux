/*
 * Copyright © 2016 Mellanox Technlogies. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <linux/slab.h>
#include <linux/genalloc.h>
#include <linux/dma-buf.h>

#include "nvme-pci.h"

struct nvme_cmb_object {
	struct nvme_dev *dev;
	struct dma_buf *dma_buf;
	void *addr;
	dma_addr_t dma_addr;
	int attachments;
	struct kref refcount;
};

static size_t obj_size(struct nvme_cmb_object *obj)
{
	return obj->dma_buf->size;
}

struct nvme_cmb_attachment {
	struct sg_table sgt;
	enum dma_data_direction dir;
};

static void nvme_cmb_object_get(struct nvme_cmb_object *obj)
{
	kref_get(&obj->refcount);
}

static void nvme_cmb_object_release(struct kref *kref)
{
	struct nvme_cmb_object *obj =
		container_of(kref, struct nvme_cmb_object, refcount);

	WARN_ON(obj->attachments);
	WARN_ON(obj->addr || obj->dma_addr);

	if (obj->dma_buf)
		dma_buf_put(obj->dma_buf);
	kfree(obj);
}

static void nvme_cmb_object_put(struct nvme_cmb_object *obj)
{
	kref_put(&obj->refcount, nvme_cmb_object_release);
}

static int nvme_cmb_map_attach(struct dma_buf *dma_buf,
			       struct device *target_dev,
			       struct dma_buf_attachment *attach)
{
	struct nvme_cmb_attachment *cmb_attach;
	struct nvme_cmb_object *obj = dma_buf->priv;
	struct nvme_dev *dev = obj->dev;
	int ret;

	cmb_attach = kzalloc(sizeof(*cmb_attach), GFP_KERNEL);
	if (!cmb_attach)
		return -ENOMEM;

	/*
	 * TODO check there is no IOMMU enabled and there is peer to peer
	 * access between target_dev and our device
	 */

	cmb_attach->dir = DMA_NONE;
	attach->priv = cmb_attach;

	if (!obj->attachments) {
		obj->addr = nvme_alloc_cmb(dev, obj_size(obj), &obj->dma_addr);
		if (!obj->addr) {
			ret = -ENOMEM;
			goto free;
		}
	}
	++obj->attachments;

	return 0;

free:
	kfree(cmb_attach);
	return ret;
}

static void nvme_cmb_map_detach(struct dma_buf *dma_buf,
				struct dma_buf_attachment *attach)
{
	struct nvme_cmb_attachment *cmb_attach = attach->priv;
	struct nvme_cmb_object *obj = dma_buf->priv;
	struct nvme_dev *dev = obj->dev;

	if (!cmb_attach)
		return;

	if (!--obj->attachments) {
		nvme_free_cmb(dev, obj->addr, obj_size(obj));
		obj->addr = NULL;
		obj->dma_addr = 0;
	}

	if (cmb_attach->dir != DMA_NONE) {
		/* TODO something like dma_unmap_resource */
		sg_free_table(&cmb_attach->sgt);
	}

	kfree(cmb_attach);
	attach->priv = NULL;
}

static struct sg_table *nvme_cmb_map_dma_buf(struct dma_buf_attachment *attach,
					     enum dma_data_direction dir)
{
	struct nvme_cmb_attachment *cmb_attach = attach->priv;
	struct nvme_cmb_object *obj = attach->dmabuf->priv;
	int ret;

	if (WARN_ON(dir == DMA_NONE || !cmb_attach))
		return ERR_PTR(-EINVAL);

	/* return the cached mapping when possible */
	if (cmb_attach->dir == dir)
		return &cmb_attach->sgt;

	/*
	 * two mappings with different directions for the same attachment are
	 * not allowed
	 */
	if (WARN_ON(cmb_attach->dir != DMA_NONE))
		return ERR_PTR(-EBUSY);

	ret = sg_alloc_table(&cmb_attach->sgt, 1, GFP_KERNEL);
	if (ret)
		return ERR_PTR(ret);

	/*
	 * TODO
	 * 1. Use something like dma_map_resource to get DMA mapping for the
	 *    BAR.
	 * 2. no struct page for this address, just a pfn. Make sure callers
	 *    don't need it.
	 */
	sg_dma_address(cmb_attach->sgt.sgl) = obj->dma_addr;
#ifdef CONFIG_NEED_SG_DMA_LENGTH
	sg_dma_len(cmb_attach->sgt.sgl) = obj_size(obj);
#endif

	cmb_attach->dir = dir;

	return &cmb_attach->sgt;
}

static void nvme_cmb_unmap_dma_buf(struct dma_buf_attachment *attach,
				   struct sg_table *sgt,
				   enum dma_data_direction dir)
{
	/* nothing to be done here */
}

static void nvme_cmb_dmabuf_release(struct dma_buf *dma_buf)
{
	struct nvme_cmb_object *obj = dma_buf->priv;

	if (!obj)
		return;

	nvme_cmb_object_put(obj);
}

static void *nvme_cmb_dmabuf_kmap_atomic(struct dma_buf *dma_buf,
					 unsigned long page_num)
{
	struct nvme_cmb_object *obj = dma_buf->priv;

	if (!obj || !obj->addr)
		return NULL;

	return obj->addr + (page_num << PAGE_SHIFT);
}

static void nvme_cmb_vm_open(struct vm_area_struct *vma)
{
	struct nvme_cmb_object *obj = vma->vm_private_data;

	nvme_cmb_object_get(obj);
}

static void nvme_cmb_vm_close(struct vm_area_struct *vma)
{
	struct nvme_cmb_object *obj = vma->vm_private_data;

	nvme_cmb_object_put(obj);
}

static int nvme_cmb_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	struct nvme_cmb_object *obj = vma->vm_private_data;
	pgoff_t offset;
	unsigned long pfn;
	int err;

	if (!obj->addr)
		return VM_FAULT_SIGBUS;

	offset = ((unsigned long)vmf->virtual_address - vma->vm_start);
	pfn = ((unsigned long)obj->addr + offset) >> PAGE_SHIFT;

	err = vm_insert_pfn(vma, (unsigned long)vmf->virtual_address, pfn);
	switch (err) {
	case -EAGAIN:
	case 0:
	case -ERESTARTSYS:
	case -EINTR:
	case -EBUSY:
		return VM_FAULT_NOPAGE;

	case -ENOMEM:
		return VM_FAULT_OOM;
	}

	return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct nvme_cmb_vm_ops = {
	.fault = nvme_cmb_fault,
	.open = nvme_cmb_vm_open,
	.close = nvme_cmb_vm_close,
};

static int nvme_cmb_dmabuf_mmap(struct dma_buf *dma_buf,
				struct vm_area_struct *vma)
{
	struct nvme_cmb_object *obj = dma_buf->priv;

	/* Check for valid size. */
	if (obj_size(obj) < vma->vm_end - vma->vm_start)
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP;
	vma->vm_ops = &nvme_cmb_vm_ops;
	vma->vm_private_data = obj;
	vma->vm_page_prot =
		pgprot_writecombine(vm_get_page_prot(vma->vm_flags));

	nvme_cmb_object_get(obj);

	return 0;
}

static const struct dma_buf_ops nvme_cmb_dmabuf_ops =  {
	.attach = nvme_cmb_map_attach,
	.detach = nvme_cmb_map_detach,
	.map_dma_buf = nvme_cmb_map_dma_buf,
	.unmap_dma_buf = nvme_cmb_unmap_dma_buf,
	.release = nvme_cmb_dmabuf_release,
	.kmap = nvme_cmb_dmabuf_kmap_atomic,
	.kmap_atomic = nvme_cmb_dmabuf_kmap_atomic,
	.mmap = nvme_cmb_dmabuf_mmap,
};

int nvme_pci_alloc_user_cmb(struct nvme_dev *dev, u64 size)
{
	struct nvme_cmb_object *obj;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int ret;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return -ENOMEM;

	kref_init(&obj->refcount);
	obj->dev = dev;

	exp_info.ops = &nvme_cmb_dmabuf_ops;
	exp_info.size = size;
	exp_info.flags = O_CLOEXEC | O_RDWR;
	exp_info.priv = obj;

	obj->dma_buf = dma_buf_export(&exp_info);
	if (IS_ERR(obj->dma_buf)) {
		ret = PTR_ERR(obj->dma_buf);
		goto put_obj;
	}

	ret = dma_buf_fd(obj->dma_buf, exp_info.flags);
	if (ret < 0)
		goto put_obj;

	return ret;

put_obj:
	nvme_cmb_object_put(obj);
	return ret;
}

