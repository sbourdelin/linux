#ifndef BUFF_POOL_H_
#define BUFF_POOL_H_

#include <linux/types.h>
#include <linux/slab.h>

struct page;
struct device;

struct buff_pool_ops {
	int (*alloc)(void *pool, unsigned long *handle);
	void (*free)(void *pool, unsigned long handle);
	unsigned int (*buff_size)(void *pool);
	unsigned int (*total_buff_size)(void *pool);
	unsigned int (*buff_headroom)(void *pool);
	unsigned int (*buff_truesize)(void *pool);
	void *(*buff_ptr)(void *pool, unsigned long handle);
	int (*buff_convert_to_page)(void *pool,
				    unsigned long handle,
				    struct page **pg, unsigned int *pg_off);
	dma_addr_t (*buff_dma)(void *pool,
			       unsigned long handle);
	void (*buff_dma_sync_cpu)(void *pool,
				  unsigned long handle,
				  unsigned int off,
				  unsigned int size);
	void (*buff_dma_sync_dev)(void *pool,
				  unsigned long handle,
				  unsigned int off,
				  unsigned int size);
	void (*destroy)(void *pool);
};

struct buff_pool {
	void *pool;
	struct buff_pool_ops *ops;
};

/* Allocates a new buffer from the pool */
static inline int bpool_alloc(struct buff_pool *pool, unsigned long *handle)
{
	return pool->ops->alloc(pool->pool, handle);
}

/* Returns a buffer originating from the pool, back to the pool */
static inline void bpool_free(struct buff_pool *pool, unsigned long handle)
{
	pool->ops->free(pool->pool, handle);
}

/* Returns the size of the buffer, w/o headroom. This is what the pool
 * creator passed to the constructor.
 */
static inline unsigned int bpool_buff_size(struct buff_pool *pool)
{
	return pool->ops->buff_size(pool->pool);
}

/* Returns the size of the buffer, plus additional headroom (if
 * any).
 */
static inline unsigned int bpool_total_buff_size(struct buff_pool *pool)
{
	return pool->ops->total_buff_size(pool->pool);
}

/* Returns additional available headroom (if any) */
static inline unsigned int bpool_buff_headroom(struct buff_pool *pool)
{
	return pool->ops->buff_headroom(pool->pool);
}

/* Returns the truesize (as for skbuff) */
static inline unsigned int bpool_buff_truesize(struct buff_pool *pool)
{
	return pool->ops->buff_truesize(pool->pool);
}

/* Returns the kernel virtual address to the handle. */
static inline void *bpool_buff_ptr(struct buff_pool *pool, unsigned long handle)
{
	return pool->ops->buff_ptr(pool->pool, handle);
}

/* Converts a handle to a page. After a successful call, the handle is
 * stale and should not be used and should be considered
 * freed. Callers need to manually clean up the returned page (using
 * page_free).
 */
static inline int bpool_buff_convert_to_page(struct buff_pool *pool,
					     unsigned long handle,
					     struct page **pg,
					     unsigned int *pg_off)
{
	return pool->ops->buff_convert_to_page(pool->pool, handle, pg, pg_off);
}

/* Returns the dma address of a buffer */
static inline dma_addr_t bpool_buff_dma(struct buff_pool *pool,
					unsigned long handle)
{
	return pool->ops->buff_dma(pool->pool, handle);
}

/* DMA sync for CPU */
static inline void bpool_buff_dma_sync_cpu(struct buff_pool *pool,
					   unsigned long handle,
					   unsigned int off,
					   unsigned int size)
{
	pool->ops->buff_dma_sync_cpu(pool->pool, handle, off, size);
}

/* DMA sync for device */
static inline void bpool_buff_dma_sync_dev(struct buff_pool *pool,
					   unsigned long handle,
					   unsigned int off,
					   unsigned int size)
{
	pool->ops->buff_dma_sync_dev(pool->pool, handle, off, size);
}

/* Destroy pool */
static inline void bpool_destroy(struct buff_pool *pool)
{
	if (!pool)
		return;

	pool->ops->destroy(pool->pool);

	kfree(pool->ops);
	kfree(pool);
}

#endif /* BUFF_POOL_H_ */

