#include "buff_pool.h"

#include "i40e.h"
#include "i40e_txrx.h"

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
};

int bpool_alloc(struct buff_pool *pool, unsigned long *handle)
{
	return pool->ops->alloc(pool->pool, handle);
}

void bpool_free(struct buff_pool *pool, unsigned long handle)
{
	pool->ops->free(pool->pool, handle);
}

unsigned int bpool_buff_size(struct buff_pool *pool)
{
	return pool->ops->buff_size(pool->pool);
}

unsigned int bpool_total_buff_size(struct buff_pool *pool)
{
	return pool->ops->total_buff_size(pool->pool);
}

unsigned int bpool_buff_headroom(struct buff_pool *pool)
{
	return pool->ops->buff_headroom(pool->pool);
}

unsigned int bpool_buff_truesize(struct buff_pool *pool)
{
	return pool->ops->buff_truesize(pool->pool);
}

void *bpool_buff_ptr(struct buff_pool *pool, unsigned long handle)
{
	return pool->ops->buff_ptr(pool->pool, handle);
}

int bpool_buff_convert_to_page(struct buff_pool *pool, unsigned long handle,
			       struct page **pg, unsigned int *pg_off)
{
	return pool->ops->buff_convert_to_page(pool->pool, handle, pg, pg_off);
}

dma_addr_t bpool_buff_dma(struct buff_pool *pool,
			  unsigned long handle)
{
	return pool->ops->buff_dma(pool->pool, handle);
}

void bpool_buff_dma_sync_cpu(struct buff_pool *pool,
			     unsigned long handle,
			     unsigned int off,
			     unsigned int size)
{
	pool->ops->buff_dma_sync_cpu(pool->pool, handle, off, size);
}

void bpool_buff_dma_sync_dev(struct buff_pool *pool,
			     unsigned long handle,
			     unsigned int off,
			     unsigned int size)
{
	pool->ops->buff_dma_sync_dev(pool->pool, handle, off, size);
}

/* Naive, non-recycling allocator. */

struct i40e_bp_pool {
	struct device *dev;
};

struct i40e_bp_header {
	dma_addr_t dma;
};

#define I40E_BPHDR_ALIGNED_SIZE ALIGN(sizeof(struct i40e_bp_header),	\
				     SMP_CACHE_BYTES)

static int i40e_bp_alloc(void *pool, unsigned long *handle)
{
	struct i40e_bp_pool *impl = (struct i40e_bp_pool *)pool;
	struct i40e_bp_header *hdr;
	struct page *pg;
	dma_addr_t dma;

	pg = dev_alloc_pages(0);
	if (unlikely(!pg))
		return -ENOMEM;

	dma = dma_map_page_attrs(impl->dev, pg, 0,
				 PAGE_SIZE,
				 DMA_FROM_DEVICE,
				 I40E_RX_DMA_ATTR);

	if (dma_mapping_error(impl->dev, dma)) {
		__free_pages(pg, 0);
		return -ENOMEM;
	}

	hdr = (struct i40e_bp_header *)page_address(pg);
	hdr->dma = dma;

	*handle = (unsigned long)(((void *)hdr) + I40E_BPHDR_ALIGNED_SIZE);

	return 0;
}

static void i40e_bp_free(void *pool, unsigned long handle)
{
	struct i40e_bp_pool *impl = (struct i40e_bp_pool *)pool;
	struct i40e_bp_header *hdr;

	hdr = (struct i40e_bp_header *)(handle & PAGE_MASK);

	dma_unmap_page_attrs(impl->dev, hdr->dma, PAGE_SIZE,
			     DMA_FROM_DEVICE, I40E_RX_DMA_ATTR);
	page_frag_free(hdr);
}

static unsigned int i40e_bp_buff_size(void *pool)
{
	(void)pool;
	return I40E_RXBUFFER_3072;
}

static unsigned int i40e_bp_total_buff_size(void *pool)
{
	(void)pool;
	return PAGE_SIZE - I40E_BPHDR_ALIGNED_SIZE -
		SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
}

static unsigned int i40e_bp_buff_headroom(void *pool)
{
	(void)pool;
	return PAGE_SIZE - I40E_BPHDR_ALIGNED_SIZE -
		SKB_DATA_ALIGN(sizeof(struct skb_shared_info)) -
		I40E_RXBUFFER_3072;
}

static unsigned int i40e_bp_buff_truesize(void *pool)
{
	(void)pool;
	return PAGE_SIZE;
}

static void *i40e_bp_buff_ptr(void *pool, unsigned long handle)
{
	return (void *)handle;
}

static int i40e_bp_buff_convert_to_page(void *pool,
					unsigned long handle,
					struct page **pg, unsigned int *pg_off)
{
	struct i40e_bp_pool *impl = (struct i40e_bp_pool *)pool;
	struct i40e_bp_header *hdr;

	hdr = (struct i40e_bp_header *)(handle & PAGE_MASK);

	dma_unmap_page_attrs(impl->dev, hdr->dma, PAGE_SIZE,
			     DMA_FROM_DEVICE, I40E_RX_DMA_ATTR);

	*pg = virt_to_page(hdr);
	*pg_off = I40E_BPHDR_ALIGNED_SIZE;

	return 0;
}

static dma_addr_t i40e_bp_buff_dma(void *pool,
				   unsigned long handle)
{
	struct i40e_bp_header *hdr;

	hdr = (struct i40e_bp_header *)(handle & PAGE_MASK);

	return hdr->dma + I40E_BPHDR_ALIGNED_SIZE;
}

static void i40e_bp_buff_dma_sync_cpu(void *pool,
				      unsigned long handle,
				      unsigned int off,
				      unsigned int size)
{
	struct i40e_bp_pool *impl = (struct i40e_bp_pool *)pool;
	struct i40e_bp_header *hdr;

	off += I40E_BPHDR_ALIGNED_SIZE;

	hdr = (struct i40e_bp_header *)(handle & PAGE_MASK);
	dma_sync_single_range_for_cpu(impl->dev, hdr->dma, off, size,
				      DMA_FROM_DEVICE);
}

static void i40e_bp_buff_dma_sync_dev(void *pool,
				      unsigned long handle,
				      unsigned int off,
				      unsigned int size)
{
	struct i40e_bp_pool *impl = (struct i40e_bp_pool *)pool;
	struct i40e_bp_header *hdr;

	off += I40E_BPHDR_ALIGNED_SIZE;

	hdr = (struct i40e_bp_header *)(handle & PAGE_MASK);
	dma_sync_single_range_for_device(impl->dev, hdr->dma, off, size,
					 DMA_FROM_DEVICE);
}

struct buff_pool *i40e_buff_pool_create(struct device *dev)
{
	struct i40e_bp_pool *pool_impl;
	struct buff_pool_ops *pool_ops;
	struct buff_pool *pool;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;

	pool_impl = kzalloc(sizeof(*pool_impl), GFP_KERNEL);
	if (!pool_impl) {
		kfree(pool);
		return NULL;
	}

	pool_ops = kzalloc(sizeof(*pool_ops), GFP_KERNEL);
	if (!pool_ops) {
		kfree(pool_impl);
		kfree(pool);
		return NULL;
	}

	pool_ops->alloc = i40e_bp_alloc;
	pool_ops->free = i40e_bp_free;
	pool_ops->buff_size = i40e_bp_buff_size;
	pool_ops->total_buff_size = i40e_bp_total_buff_size;
	pool_ops->buff_headroom = i40e_bp_buff_headroom;
	pool_ops->buff_truesize = i40e_bp_buff_truesize;
	pool_ops->buff_ptr = i40e_bp_buff_ptr;
	pool_ops->buff_convert_to_page = i40e_bp_buff_convert_to_page;
	pool_ops->buff_dma = i40e_bp_buff_dma;
	pool_ops->buff_dma_sync_cpu = i40e_bp_buff_dma_sync_cpu;
	pool_ops->buff_dma_sync_dev = i40e_bp_buff_dma_sync_dev;

	pool_impl->dev = dev;

	pool->pool = pool_impl;
	pool->ops = pool_ops;

	return pool;
}

void i40e_buff_pool_destroy(struct buff_pool *pool)
{
	kfree(pool->ops);
	kfree(pool->pool);
	kfree(pool);
}

