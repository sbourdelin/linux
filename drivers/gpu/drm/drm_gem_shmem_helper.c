/*
 * drm gem shmem (shared memory) helper functions
 *
 * Copyright (C) 2017 Noralf Trønnes
 *
 * Based on drm_gem_cma_helper.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/dma-buf.h>
#include <linux/shmem_fs.h>

#include <drm/drmP.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vma_manager.h>

/**
 * DOC: shmem helpers
 *
 * The Contiguous Memory Allocator reserves a pool of memory at early boot
 * that is used to service requests for large blocks of contiguous memory.
 *
 * The DRM GEM/SHMEM helpers use this allocator as a means to provide buffer
 * objects that are physically contiguous in memory. This is useful for
 * display drivers that are unable to map scattered buffers via an IOMMU.
 */

static struct drm_gem_shmem_object *
__drm_gem_shmem_create(struct drm_device *drm, size_t size)
{
	struct drm_gem_shmem_object *obj;
	struct drm_gem_object *gem_obj;
	int ret;

	if (drm->driver->gem_create_object)
		gem_obj = drm->driver->gem_create_object(drm, size);
	else
		gem_obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!gem_obj)
		return ERR_PTR(-ENOMEM);

	obj = to_drm_gem_shmem_obj(gem_obj);

	if (!drm->driver->gem_create_object)
		obj->cache_mode = DRM_GEM_SHMEM_BO_WRITECOMBINED;

	ret = drm_gem_object_init(drm, gem_obj, size);
	if (ret)
		goto error;

	ret = drm_gem_create_mmap_offset(gem_obj);
	if (ret) {
		drm_gem_object_release(gem_obj);
		goto error;
	}

	return obj;

error:
	kfree(obj);

	return ERR_PTR(ret);
}

/**
 * drm_gem_shmem_create - allocate an object with the given size
 * @drm: DRM device
 * @size: size of the object to allocate
 *
 * This function creates a shmem GEM object and uses drm_gem_get_pages() to get
 * the backing pages.
 *
 * Returns:
 * A struct drm_gem_shmem_object * on success or an ERR_PTR()-encoded negative
 * error code on failure.
 */
struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *drm,
						  size_t size)
{
	struct drm_gem_shmem_object *obj;
	int ret;

	size = round_up(size, PAGE_SIZE);

	obj = __drm_gem_shmem_create(drm, size);
	if (IS_ERR(obj))
		return obj;

	obj->pages = drm_gem_get_pages(&obj->base);
	if (IS_ERR(obj->pages)) {
		dev_err(drm->dev, "failed to allocate buffer with size %zu\n",
			size);
		ret = PTR_ERR(obj->pages);
		goto error;
	}

	return obj;

error:
	drm_gem_object_put_unlocked(&obj->base);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_create);

static struct drm_gem_shmem_object *
drm_gem_shmem_create_with_handle(struct drm_file *file_priv,
				 struct drm_device *drm, size_t size,
				 uint32_t *handle)
{
	struct drm_gem_shmem_object *obj;
	struct drm_gem_object *gem_obj;
	int ret;

	obj = drm_gem_shmem_create(drm, size);
	if (IS_ERR(obj))
		return obj;

	gem_obj = &obj->base;

	/*
	 * allocate a id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file_priv, gem_obj, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put_unlocked(gem_obj);
	if (ret)
		return ERR_PTR(ret);

	return obj;
}

/**
 * drm_gem_shmem_free_object - free resources associated with a shmem GEM object
 * @gem_obj: GEM object to free
 *
 * This function frees the backing memory of the shmem GEM object, cleans up
 * the GEM object state and frees the memory used to store the object itself.
 * Drivers using the shmem helpers should set this as their
 * &drm_driver.gem_free_object callback.
 */
void drm_gem_shmem_free_object(struct drm_gem_object *gem_obj)
{
	struct drm_gem_shmem_object *obj = to_drm_gem_shmem_obj(gem_obj);

	drm_gem_shmem_vunmap(obj);

	if (gem_obj->import_attach) {
		drm_prime_gem_destroy(gem_obj, obj->sgt);
		kvfree(obj->pages);
	} else {
		drm_gem_put_pages(&obj->base, obj->pages, false, false);
	}

	drm_gem_object_release(gem_obj);

	kfree(obj);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_free_object);

/*
 * drm_gem_shmem_vmap - vmap a shmem GEM object
 * @obj: shmem GEM object
 *
 * This function makes sure that a virtual address exists for
 * the shmem GEM object.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int drm_gem_shmem_vmap(struct drm_gem_shmem_object *obj)
{
	struct drm_gem_object *gem_obj = &obj->base;

	if (obj->vaddr)
		return 0;

	/* TODO: locking? */

	if (gem_obj->import_attach) {
		obj->vaddr = dma_buf_vmap(gem_obj->import_attach->dmabuf);
	} else {
		pgprot_t prot;

		switch (obj->cache_mode) {
		case DRM_GEM_SHMEM_BO_WRITECOMBINED:
			prot = pgprot_writecombine(PAGE_KERNEL);
			break;

		case DRM_GEM_SHMEM_BO_UNCACHED:
			prot = pgprot_noncached(PAGE_KERNEL);
			break;

		case DRM_GEM_SHMEM_BO_CACHED:
			prot = PAGE_KERNEL;
			break;
		}

		obj->vaddr = vmap(obj->pages, gem_obj->size >> PAGE_SHIFT,
				  VM_MAP, prot);
	}

	if (!obj->vaddr)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL(drm_gem_shmem_vmap);

/*
 * drm_gem_shmem_vunmap - vunmap a shmem GEM object
 * @obj: shmem GEM object
 *
 * This function makes sure that the virtual address is removed for
 * the shmem GEM object.
 */
void drm_gem_shmem_vunmap(struct drm_gem_shmem_object *obj)
{
	struct drm_gem_object *gem_obj = &obj->base;

	if (!obj->vaddr)
		return;

	if (gem_obj->import_attach)
		dma_buf_vunmap(gem_obj->import_attach->dmabuf, obj->vaddr);
	else
		vunmap(obj->vaddr);

	obj->vaddr = NULL;
}
EXPORT_SYMBOL(drm_gem_shmem_vunmap);

/**
 * drm_gem_shmem_dumb_create - create a dumb shmem buffer object
 * @file_priv: DRM file structure to create the dumb buffer for
 * @drm: DRM device
 * @args: IOCTL data
 *
 * This function computes the pitch of the dumb buffer and rounds it up to an
 * integer number of bytes per pixel. Drivers for hardware that doesn't have
 * any additional restrictions on the pitch can directly use this function as
 * their &drm_driver.dumb_create callback.
 *
 * For hardware with additional restrictions, drivers can adjust the fields
 * set up by userspace before calling into this function.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_dumb_create(struct drm_file *file_priv,
			      struct drm_device *drm,
			      struct drm_mode_create_dumb *args)
{
	struct drm_gem_shmem_object *obj;

	if (!args->pitch || !args->size) {
		args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
		args->size = args->pitch * args->height;
	} else {
		/* ensure sane minimum values */
		args->pitch = max(args->pitch,
				  DIV_ROUND_UP(args->width * args->bpp, 8));
		args->size = max_t(typeof(args->size), args->size,
				   args->pitch * args->height);
	}

	obj = drm_gem_shmem_create_with_handle(file_priv, drm, args->size,
					       &args->handle);
	return PTR_ERR_OR_ZERO(obj);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_dumb_create);

static int drm_gem_shmem_fault(struct vm_fault *vmf)
{
	struct vm_area_struct *vma = vmf->vma;
	struct drm_gem_shmem_object *obj = to_drm_gem_shmem_obj(vma->vm_private_data);
	/* We don't use vmf->pgoff since that has the fake offset */
	unsigned long vaddr = vmf->address;
	struct page *page;

	page = shmem_read_mapping_page(file_inode(obj->base.filp)->i_mapping,
				       (vaddr - vma->vm_start) >> PAGE_SHIFT);
	if (!IS_ERR(page)) {
		vmf->page = page;
		return 0;
	} else switch (PTR_ERR(page)) {
		case -ENOSPC:
		case -ENOMEM:
			return VM_FAULT_OOM;
		case -EBUSY:
			return VM_FAULT_RETRY;
		case -EFAULT:
		case -EINVAL:
			return VM_FAULT_SIGBUS;
		default:
			WARN_ON_ONCE(PTR_ERR(page));
			return VM_FAULT_SIGBUS;
	}
}

const struct vm_operations_struct drm_gem_shmem_vm_ops = {
	.fault = drm_gem_shmem_fault,
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};
EXPORT_SYMBOL_GPL(drm_gem_shmem_vm_ops);

static int drm_gem_shmem_mmap_obj(struct drm_gem_shmem_object *obj,
				  struct vm_area_struct *vma)
{
	/* VM_PFNMAP was set by drm_gem_mmap() */
	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_flags |= VM_MIXEDMAP;

	switch (obj->cache_mode) {
	case DRM_GEM_SHMEM_BO_WRITECOMBINED:
		vma->vm_page_prot =
			pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
		break;

	case DRM_GEM_SHMEM_BO_UNCACHED:
		vma->vm_page_prot =
			pgprot_noncached(vm_get_page_prot(vma->vm_flags));
		break;

	case DRM_GEM_SHMEM_BO_CACHED:
		/*
		 * Shunt off cached objs to shmem file so they have their own
		 * address_space (so unmap_mapping_range does what we want,
		 * in particular in the case of mmap'd dmabufs)
		 */
		fput(vma->vm_file);
		get_file(obj->base.filp);
		vma->vm_pgoff = 0;
		vma->vm_file  = obj->base.filp;

		vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
		break;
	}

	return 0;
}

/**
 * drm_gem_shmem_mmap - memory-map a shmem GEM object
 * @filp: file object
 * @vma: VMA for the area to be mapped
 *
 * This function implements an augmented version of the GEM DRM file mmap
 * operation for shmem objects. Drivers which employ the shmem helpers should
 * use this function as their ->mmap() handler in the DRM device file's
 * file_operations structure.
 *
 * Instead of directly referencing this function, drivers should use the
 * DEFINE_DRM_GEM_SHMEM_FOPS().macro.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_mmap(struct file *filp, struct vm_area_struct *vma)
{

	struct drm_gem_shmem_object *obj;
	struct drm_gem_object *gem_obj;
	int ret;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	gem_obj = vma->vm_private_data;
	obj = to_drm_gem_shmem_obj(gem_obj);

	return drm_gem_shmem_mmap_obj(obj, vma);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_mmap);

#ifndef CONFIG_MMU
/**
 * drm_gem_shmem_get_unmapped_area - propose address for mapping in noMMU cases
 * @filp: file object
 * @addr: memory address
 * @len: buffer size
 * @pgoff: page offset
 * @flags: memory flags
 *
 * This function is used in noMMU platforms to propose address mapping
 * for a given buffer.
 * It's intended to be used as a direct handler for the struct
 * &file_operations.get_unmapped_area operation.
 *
 * Returns:
 * mapping address on success or a negative error code on failure.
 */
unsigned long drm_gem_shmem_get_unmapped_area(struct file *filp,
					      unsigned long addr,
					      unsigned long len,
					      unsigned long pgoff,
					      unsigned long flags)
{
	struct drm_file *priv = filp->private_data;
	struct drm_device *dev = priv->minor->dev;
	struct drm_gem_object *gem_obj = NULL;
	struct drm_vma_offset_node *node;
	struct drm_gem_shmem_object *obj;

	if (drm_device_is_unplugged(dev))
		return -ENODEV;

	drm_vma_offset_lock_lookup(dev->vma_offset_manager);
	node = drm_vma_offset_exact_lookup_locked(dev->vma_offset_manager,
						  pgoff,
						  len >> PAGE_SHIFT);
	if (likely(node)) {
		gem_obj = container_of(node, struct drm_gem_object, vma_node);
		/*
		 * When the object is being freed, after it hits 0-refcnt it
		 * proceeds to tear down the object. In the process it will
		 * attempt to remove the VMA offset and so acquire this
		 * mgr->vm_lock.  Therefore if we find an object with a 0-refcnt
		 * that matches our range, we know it is in the process of being
		 * destroyed and will be freed as soon as we release the lock -
		 * so we have to check for the 0-refcnted object and treat it as
		 * invalid.
		 */
		if (!kref_get_unless_zero(&gem_obj->refcount))
			gem_obj = NULL;
	}

	drm_vma_offset_unlock_lookup(dev->vma_offset_manager);

	if (!gem_obj)
		return -EINVAL;

	if (!drm_vma_node_is_allowed(node, priv)) {
		drm_gem_object_put_unlocked(gem_obj);
		return -EACCES;
	}

	obj = to_drm_gem_shmem_obj(gem_obj);

	drm_gem_object_put_unlocked(gem_obj);

	/* TODO: should we call drm_gem_shmem_vmap() here? */

	return obj->vaddr ? (unsigned long)obj->vaddr : -EINVAL;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_get_unmapped_area);
#endif

#ifdef CONFIG_DEBUG_FS
static const char *cache_mode_str[] = {
	[DRM_GEM_SHMEM_BO_UNCACHED] = "uncached",
	[DRM_GEM_SHMEM_BO_CACHED] = "cached",
	[DRM_GEM_SHMEM_BO_WRITECOMBINED] = "writecombined",
};

/**
 * drm_gem_shmem_describe - describe a shmem GEM object for debugfs
 * @obj: shmem GEM object
 * @m: debugfs file handle
 *
 * This function can be used to dump a human-readable representation of the
 * shmem GEM object into a synthetic file.
 */
void drm_gem_shmem_describe(struct drm_gem_shmem_object *obj,
			    struct seq_file *m)
{
	struct drm_gem_object *gem_obj = &obj->base;
	uint64_t off;

	off = drm_vma_node_start(&gem_obj->vma_node);
	seq_printf(m, "name=%d refcount=%d off=%08llx vaddr=%p size=%zu mode=%s",
		   gem_obj->name, kref_read(&gem_obj->refcount), off,
		   obj->vaddr, gem_obj->size, cache_mode_str[obj->cache_mode]);

	seq_printf(m, "\n");
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_describe);
#endif

/**
 * drm_gem_shmem_prime_get_sg_table - provide a scatter/gather table of pinned
 *                                    pages for a SHMEM GEM object
 * @obj: GEM object
 *
 * This function exports a scatter/gather table suitable for PRIME usage by
 * calling the standard DMA mapping API. Drivers using the shmem helpers should
 * set this as their &drm_driver.gem_prime_get_sg_table callback.
 *
 * Returns:
 * A pointer to the scatter/gather table of pinned pages or NULL on failure.
 */
struct sg_table *
drm_gem_shmem_prime_get_sg_table(struct drm_gem_object *gem_obj)
{
	struct drm_gem_shmem_object *obj = to_drm_gem_shmem_obj(gem_obj);

	return drm_prime_pages_to_sg(obj->pages, gem_obj->size >> PAGE_SHIFT);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_get_sg_table);

/**
 * drm_gem_shmem_prime_import_sg_table - produce a shmem GEM object from
 *                 another driver's scatter/gather table of pinned pages
 * @dev: device to import into
 * @attach: DMA-BUF attachment
 * @sgt: scatter/gather table of pinned pages
 *
 * This function imports a scatter/gather table exported via DMA-BUF by
 * another driver. Drivers that use the shmem helpers should set this as their
 * &drm_driver.gem_prime_import_sg_table callback.
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object *
drm_gem_shmem_prime_import_sg_table(struct drm_device *dev,
				    struct dma_buf_attachment *attach,
				    struct sg_table *sgt)
{
	int npages = attach->dmabuf->size >> PAGE_SHIFT;
	struct drm_gem_shmem_object *obj;
	int ret;

	obj = __drm_gem_shmem_create(dev, attach->dmabuf->size);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	obj->pages = kvmalloc_array(npages, sizeof(struct page *), GFP_KERNEL);
	if (!obj->pages) {
		ret = -ENOMEM;
		goto err_free_gem;
	}

	ret = drm_prime_sg_to_page_addr_arrays(sgt, obj->pages, NULL, npages);
	if (ret < 0)
		goto err_free_array;

	obj->sgt = sgt;

	DRM_DEBUG_PRIME("size = %zu\n", attach->dmabuf->size);

	return &obj->base;

err_free_array:
	kvfree(obj->pages);
err_free_gem:
	drm_gem_object_put_unlocked(&obj->base);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_import_sg_table);

/**
 * drm_gem_shmem_prime_mmap - memory-map an exported shmem GEM object
 * @obj: GEM object
 * @vma: VMA for the area to be mapped
 *
 * This function maps a buffer imported via DRM PRIME into a userspace
 * process's address space. Drivers that use the shmem helpers should set this
 * as their &drm_driver.gem_prime_mmap callback.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_gem_shmem_prime_mmap(struct drm_gem_object *gem_obj,
			     struct vm_area_struct *vma)
{
	struct drm_gem_shmem_object *obj = to_drm_gem_shmem_obj(gem_obj);
	int ret;

	ret = drm_gem_mmap_obj(gem_obj, gem_obj->size, vma);
	if (ret < 0)
		return ret;

	return drm_gem_shmem_mmap_obj(obj, vma);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_mmap);

/**
 * drm_gem_shmem_prime_vmap - map a shmem GEM object into the kernel's virtual
 *                            address space
 * @obj: GEM object
 *
 * This function maps a buffer exported via DRM PRIME into the kernel's
 * virtual address space. Drivers using the SHMEM helpers should set this as
 * their DRM driver's &drm_driver.gem_prime_vmap callback.
 *
 * Returns:
 * The kernel virtual address of the SHMEM GEM object's backing store.
 */
void *drm_gem_shmem_prime_vmap(struct drm_gem_object *gem_obj)
{
	struct drm_gem_shmem_object *obj = to_drm_gem_shmem_obj(gem_obj);

	drm_gem_shmem_vmap(obj);

	return obj->vaddr;
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_vmap);

/**
 * drm_gem_shmem_prime_vunmap - unmap a shmem GEM object from the kernel's
 *                              virtual address space
 * @obj: GEM object
 * @vaddr: kernel virtual address where the shmem GEM object was mapped
 *
 * This function removes a buffer exported via DRM PRIME from the kernel's
 * virtual address space. Drivers using the SHMEM helpers should set this as
 * their &drm_driver.gem_prime_vunmap callback.
 */
void drm_gem_shmem_prime_vunmap(struct drm_gem_object *gem_obj, void *vaddr)
{
	struct drm_gem_shmem_object *obj = to_drm_gem_shmem_obj(gem_obj);

	drm_gem_shmem_vunmap(obj);
}
EXPORT_SYMBOL_GPL(drm_gem_shmem_prime_vunmap);
