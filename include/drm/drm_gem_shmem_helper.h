#ifndef __DRM_GEM_SHMEM_HELPER_H__
#define __DRM_GEM_SHMEM_HELPER_H__

#include <drm/drmP.h>
#include <drm/drm_gem.h>

enum drm_gem_shmem_cache_mode {
	DRM_GEM_SHMEM_BO_UNCACHED,
	DRM_GEM_SHMEM_BO_CACHED,
	DRM_GEM_SHMEM_BO_WRITECOMBINED,
};

/**
 * struct drm_gem_shmem_object - GEM object backed by shmem
 * @base: base GEM object
 * @pages: page table
 * @cache_mode: Cache mode
 * @sgt: scatter/gather table for imported PRIME buffers
 * @vaddr: kernel virtual address of the backing memory
 */
struct drm_gem_shmem_object {
	struct drm_gem_object base;
	struct page **pages;
	enum drm_gem_shmem_cache_mode cache_mode;
	struct sg_table *sgt;
	void *vaddr;
};

static inline struct drm_gem_shmem_object *
to_drm_gem_shmem_obj(struct drm_gem_object *gem_obj)
{
	return container_of(gem_obj, struct drm_gem_shmem_object, base);
}

#ifndef CONFIG_MMU
#define DRM_GEM_SHMEM_UNMAPPED_AREA_FOPS \
	.get_unmapped_area	= drm_gem_shmem_get_unmapped_area,
#else
#define DRM_GEM_SHMEM_UNMAPPED_AREA_FOPS
#endif

/**
 * DEFINE_DRM_GEM_SHMEM_FOPS() - macro to generate file operations for shmem
 *                               drivers
 * @name: name for the generated structure
 *
 * This macro autogenerates a suitable &struct file_operations for shmem based
 * drivers, which can be assigned to &drm_driver.fops. Note that this structure
 * cannot be shared between drivers, because it contains a reference to the
 * current module using THIS_MODULE.
 *
 * Note that the declaration is already marked as static - if you need a
 * non-static version of this you're probably doing it wrong and will break the
 * THIS_MODULE reference by accident.
 */
#define DEFINE_DRM_GEM_SHMEM_FOPS(name) \
	static const struct file_operations name = {\
		.owner		= THIS_MODULE,\
		.open		= drm_open,\
		.release	= drm_release,\
		.unlocked_ioctl	= drm_ioctl,\
		.compat_ioctl	= drm_compat_ioctl,\
		.poll		= drm_poll,\
		.read		= drm_read,\
		.llseek		= noop_llseek,\
		.mmap		= drm_gem_shmem_mmap,\
		DRM_GEM_SHMEM_UNMAPPED_AREA_FOPS \
	}

struct drm_gem_shmem_object *drm_gem_shmem_create(struct drm_device *drm,
						  size_t size);
void drm_gem_shmem_free_object(struct drm_gem_object *gem_obj);

int drm_gem_shmem_vmap(struct drm_gem_shmem_object *obj);
void drm_gem_shmem_vunmap(struct drm_gem_shmem_object *obj);

int drm_gem_shmem_dumb_create(struct drm_file *file_priv,
			      struct drm_device *drm,
			      struct drm_mode_create_dumb *args);

int drm_gem_shmem_mmap(struct file *filp, struct vm_area_struct *vma);

extern const struct vm_operations_struct drm_gem_shmem_vm_ops;

#ifndef CONFIG_MMU
unsigned long drm_gem_shmem_get_unmapped_area(struct file *filp,
					      unsigned long addr,
					      unsigned long len,
					      unsigned long pgoff,
					      unsigned long flags);
#endif

#ifdef CONFIG_DEBUG_FS
void drm_gem_shmem_describe(struct drm_gem_shmem_object *obj,
			    struct seq_file *m);
#endif

struct sg_table *
drm_gem_shmem_prime_get_sg_table(struct drm_gem_object *gem_obj);
struct drm_gem_object *
drm_gem_shmem_prime_import_sg_table(struct drm_device *dev,
				    struct dma_buf_attachment *attach,
				    struct sg_table *sgt);
int drm_gem_shmem_prime_mmap(struct drm_gem_object *gem_obj,
			     struct vm_area_struct *vma);
void *drm_gem_shmem_prime_vmap(struct drm_gem_object *gem_obj);
void drm_gem_shmem_prime_vunmap(struct drm_gem_object *gem_obj, void *vaddr);

/**
 * DRM_GEM_SHMEM_DRIVER_OPS - default shmem gem operations
 *
 * This macro provides a shortcut for setting the shmem GEM operations in
 * the &drm_driver structure.
 */
#define DRM_GEM_SHMEM_DRIVER_OPS \
	.gem_free_object	= drm_gem_shmem_free_object, \
	.gem_vm_ops		= &drm_gem_shmem_vm_ops, \
	.prime_handle_to_fd	= drm_gem_prime_handle_to_fd, \
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle, \
	.gem_prime_import	= drm_gem_prime_import, \
	.gem_prime_export	= drm_gem_prime_export, \
	.gem_prime_get_sg_table	= drm_gem_shmem_prime_get_sg_table, \
	.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table, \
	.gem_prime_vmap		= drm_gem_shmem_prime_vmap, \
	.gem_prime_vunmap	= drm_gem_shmem_prime_vunmap, \
	.gem_prime_mmap		= drm_gem_shmem_prime_mmap, \
	.dumb_create		= drm_gem_shmem_dumb_create, \
	.dumb_map_offset	= drm_gem_dumb_map_offset, \
	.dumb_destroy		= drm_gem_dumb_destroy

#endif /* __DRM_GEM_SHMEM_HELPER_H__ */
