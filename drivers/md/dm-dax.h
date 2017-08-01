#ifndef __DM_DAX_H__
#define __DM_DAX_H__
#include <linux/dax.h>
#if IS_ENABLED(CONFIG_DM_DAX)
/* dax helpers to allow compiling out dax support */
long linear_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn);
size_t linear_dax_copy_from_iter(struct dm_target *ti, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i);
void linear_dax_flush(struct dm_target *ti, pgoff_t pgoff, void *addr,
		size_t size);
long origin_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn);
long stripe_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn);
size_t stripe_dax_copy_from_iter(struct dm_target *ti, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i);
void stripe_dax_flush(struct dm_target *ti, pgoff_t pgoff, void *addr,
		size_t size);
long io_err_dax_direct_access(struct dm_target *ti, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn);
static inline struct dax_device *dm_dax_get_by_host(const char *host)
{
	return dax_get_by_host(host);
}
static inline void dm_put_dax(struct dax_device *dax_dev)
{
	put_dax(dax_dev);
}
static inline struct dax_device *dm_alloc_dax(void *p, const char *host,
		const struct dax_operations *ops)
{
	return alloc_dax(p, host, ops);
}
static inline void dm_kill_dax(struct dax_device *dax_dev)
{
	kill_dax(dax_dev);
}
long dm_dax_direct_access(struct dax_device *dax_dev, pgoff_t pgoff,
		long nr_pages, void **kaddr, pfn_t *pfn);
size_t dm_dax_copy_from_iter(struct dax_device *dax_dev, pgoff_t pgoff,
		void *addr, size_t bytes, struct iov_iter *i);
void dm_dax_flush(struct dax_device *dax_dev, pgoff_t pgoff, void *addr,
		size_t size);
#else
#define linear_dax_direct_access NULL
#define linear_dax_copy_from_iter NULL
#define linear_dax_flush NULL
#define origin_dax_direct_access NULL
#define stripe_dax_direct_access NULL
#define stripe_dax_copy_from_iter NULL
#define stripe_dax_flush NULL
#define io_err_dax_direct_access NULL
static inline struct dax_device *dm_dax_get_by_host(const char *host)
{
	return NULL;
}
static inline void dm_put_dax(struct dax_device *dax_dev)
{
}
static inline struct dax_device *dm_alloc_dax(void *private, const char *__host,
		const struct dax_operations *ops)
{
	return NULL;
}
static inline void dm_kill_dax(struct dax_device *dax_dev)
{
}
#define dm_dax_direct_access NULL
#define dm_dax_copy_from_iter NULL
#define dm_dax_flush NULL
#endif
#endif /* __DM_DAX_H__ */
