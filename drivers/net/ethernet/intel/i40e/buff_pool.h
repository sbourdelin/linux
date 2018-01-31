#ifndef BUFF_POOL_H_
#define BUFF_POOL_H_

#include <linux/types.h>

struct page;
struct device;

struct buff_pool_ops;

struct buff_pool {
	void *pool;
	struct buff_pool_ops *ops;
};

/* Allocates a new buffer from the pool */
int bpool_alloc(struct buff_pool *pool, unsigned long *handle);

/* Returns a buffer originating from the pool, back to the pool */
void bpool_free(struct buff_pool *pool, unsigned long handle);

/* Returns the size of the buffer, w/o headroom. This is what the pool
 * creator passed to the constructor.
 */
unsigned int bpool_buff_size(struct buff_pool *pool);

/* Returns the size of the buffer, plus additional headroom (if
 * any).
 */
unsigned int bpool_total_buff_size(struct buff_pool *pool);

/* Returns additional headroom (if any) */
unsigned int bpool_buff_headroom(struct buff_pool *pool);

/* Returns the truesize (as for skbuff) */
unsigned int bpool_buff_truesize(struct buff_pool *pool);

/* Returns the kernel virtual address to the handle. */
void *bpool_buff_ptr(struct buff_pool *pool, unsigned long handle);

/* Converts a handle to a page. After a successful call, the handle is
 * stale and should not be used and should be considered
 * freed. Callers need to manually clean up the returned page (using
 * page_free).
 */
int bpool_buff_convert_to_page(struct buff_pool *pool, unsigned long handle,
			       struct page **pg, unsigned int *pg_off);

/* Returns the dma address of a buffer */
dma_addr_t bpool_buff_dma(struct buff_pool *pool,
			  unsigned long handle);

/* DMA sync for CPU */
void bpool_buff_dma_sync_cpu(struct buff_pool *pool,
			     unsigned long handle,
			     unsigned int off,
			     unsigned int size);

/* DMA sync for device */
void bpool_buff_dma_sync_dev(struct buff_pool *pool,
			     unsigned long handle,
			     unsigned int off,
			     unsigned int size);
/* ---- */

struct buff_pool *i40e_buff_pool_create(struct device *dev);
void i40e_buff_pool_destroy(struct buff_pool *pool);

struct buff_pool *i40e_buff_pool_recycle_create(unsigned int mtu,
						bool reserve_headroom,
						struct device *dev,
						unsigned int pool_size);
void i40e_buff_pool_recycle_destroy(struct buff_pool *pool);

#endif /* BUFF_POOL_H_ */

