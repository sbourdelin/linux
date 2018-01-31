#include "i40e_buff_pool.h"

#include <linux/buff_pool.h>

#include "i40e.h"
#include "i40e_txrx.h"

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

static void i40e_bp_destroy(void *pool)
{
	kfree(pool);
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
	pool_ops->destroy = i40e_bp_destroy;

	pool_impl->dev = dev;

	pool->pool = pool_impl;
	pool->ops = pool_ops;

	return pool;
}

/* Recycling allocator */

struct i40e_bpr_header {
	dma_addr_t dma;
#if (BITS_PER_LONG > 32) || (PAGE_SIZE >= 65536)
	__u32 page_offset;
#else
	__u16 page_offset;
#endif
	__u16 pagecnt_bias;
};

struct i40e_bpr_pool {
	unsigned int buff_tot_len;
	unsigned int buff_len;
	unsigned int headroom;
	unsigned int pg_order;
	unsigned int pg_size;
	struct device *dev;
	unsigned int head;
	unsigned int tail;
	unsigned int buffs_size_mask;
	struct i40e_bpr_header *buffs[0];
};

#define I40E_BPRHDR_ALIGNED_SIZE ALIGN(sizeof(struct i40e_bpr_header),	\
				       SMP_CACHE_BYTES)

static int i40e_bpr_alloc(void *pool, unsigned long *handle)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;
	struct i40e_bpr_header *hdr;
	struct page *pg;
	dma_addr_t dma;

	if (impl->head != impl->tail) {
		*handle = (unsigned long)impl->buffs[impl->head];
		impl->head = (impl->head + 1) & impl->buffs_size_mask;

		return 0;
	}

	pg = dev_alloc_pages(impl->pg_order);
	if (unlikely(!pg))
		return -ENOMEM;

	dma = dma_map_page_attrs(impl->dev, pg, 0, impl->pg_size,
				 DMA_FROM_DEVICE, I40E_RX_DMA_ATTR);

	if (dma_mapping_error(impl->dev, dma)) {
		__free_pages(pg, impl->pg_order);
		return -ENOMEM;
	}

	hdr = (struct i40e_bpr_header *)page_address(pg);
	hdr->dma = dma;
	hdr->page_offset = I40E_BPRHDR_ALIGNED_SIZE;
	hdr->pagecnt_bias = 1;

	*handle = (unsigned long)hdr;

	return 0;
}

static void i40e_bpr_free(void *pool, unsigned long handle)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;
	struct i40e_bpr_header *hdr;
	unsigned int tail;

	hdr = (struct i40e_bpr_header *)handle;
	tail = (impl->tail + 1) & impl->buffs_size_mask;
	/* Is full? */
	if (tail == impl->head) {
		dma_unmap_page_attrs(impl->dev, hdr->dma, impl->pg_size,
				     DMA_FROM_DEVICE, I40E_RX_DMA_ATTR);
		__page_frag_cache_drain(virt_to_head_page(hdr),
					hdr->pagecnt_bias);
	}

	impl->buffs[impl->tail] = hdr;
	impl->tail = tail;
}

static unsigned int i40e_bpr_buff_size(void *pool)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;

	return impl->buff_len;
}

static unsigned int i40e_bpr_total_buff_size(void *pool)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;

	return impl->buff_tot_len;
}

static unsigned int i40e_bpr_buff_headroom(void *pool)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;

	return impl->headroom;
}

static unsigned int i40e_bpr_buff_truesize(void *pool)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;

	return impl->buff_tot_len;
}

static void *i40e_bpr_buff_ptr(void *pool, unsigned long handle)
{
	struct i40e_bpr_header *hdr;

	hdr = (struct i40e_bpr_header *)handle;

	return ((void *)hdr) + hdr->page_offset;
}

static bool i40e_page_is_reusable(struct page *page)
{
	return (page_to_nid(page) == numa_mem_id()) &&
		!page_is_pfmemalloc(page);
}

static bool i40e_can_reuse_page(struct i40e_bpr_header *hdr)
{
	unsigned int pagecnt_bias = hdr->pagecnt_bias;
	struct page *page = virt_to_head_page(hdr);

	if (unlikely(!i40e_page_is_reusable(page)))
		return false;

#if (PAGE_SIZE < 8192)
	if (unlikely((page_count(page) - pagecnt_bias) > 1))
		return false;
#else
#define I40E_LAST_OFFSET \
	(PAGE_SIZE - I40E_RXBUFFER_3072 - I40E_BPRHDR_ALIGNED_SIZE)
	if (hdr->page_offset > I40E_LAST_OFFSET)
		return false;
#endif

	if (unlikely(!pagecnt_bias)) {
		page_ref_add(page, USHRT_MAX);
		hdr->pagecnt_bias = USHRT_MAX;
	}

	return true;
}

static int i40e_bpr_buff_convert_to_page(void *pool, unsigned long handle,
					 struct page **pg,
					 unsigned int *pg_off)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;
	struct i40e_bpr_header *hdr;
	unsigned int tail;

	hdr = (struct i40e_bpr_header *)handle;

	*pg = virt_to_page(hdr);
	*pg_off = hdr->page_offset;

#if (PAGE_SIZE < 8192)
	hdr->page_offset ^= impl->buff_tot_len;
#else
	hdr->page_offset += impl->buff_tot_len;
#endif
	hdr->pagecnt_bias--;

	tail = (impl->tail + 1) & impl->buffs_size_mask;
	if (i40e_can_reuse_page(hdr) && tail != impl->head) {
		impl->buffs[impl->tail] = hdr;
		impl->tail = tail;

		return 0;
	}

	dma_unmap_page_attrs(impl->dev, hdr->dma, impl->pg_size,
			     DMA_FROM_DEVICE, I40E_RX_DMA_ATTR);
	__page_frag_cache_drain(*pg, hdr->pagecnt_bias);
	return 0;
}

static inline dma_addr_t i40e_bpr_buff_dma(void *pool,
					   unsigned long handle)
{
	struct i40e_bpr_header *hdr;

	hdr = (struct i40e_bpr_header *)handle;

	return hdr->dma + hdr->page_offset;
}

static void i40e_bpr_buff_dma_sync_cpu(void *pool,
				       unsigned long handle,
				       unsigned int off,
				       unsigned int size)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;
	dma_addr_t dma;

	dma = i40e_bpr_buff_dma(pool, handle);
	dma_sync_single_range_for_cpu(impl->dev, dma, off, size,
				      DMA_FROM_DEVICE);
}

static void i40e_bpr_buff_dma_sync_dev(void *pool,
				       unsigned long handle,
				       unsigned int off,
				       unsigned int size)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;
	dma_addr_t dma;

	dma = i40e_bpr_buff_dma(pool, handle);
	dma_sync_single_range_for_device(impl->dev, dma, off, size,
					 DMA_FROM_DEVICE);
}

static void calc_buffer_size_less_8192(unsigned int mtu, bool reserve_headroom,
				       unsigned int *buff_tot_len,
				       unsigned int *buff_len,
				       unsigned int *headroom,
				       unsigned int *pg_order)
{
	*pg_order = 0;

	if (!reserve_headroom) {
		*buff_tot_len = (PAGE_SIZE - I40E_BPRHDR_ALIGNED_SIZE) / 2;
		*buff_len = *buff_tot_len;
		*headroom = 0;

		return;
	}

	/* We're relying on page flipping, so make sure that a page
	 * (with the buff header removed) / 2 is large enough.
	 */
	*buff_tot_len = (PAGE_SIZE - I40E_BPRHDR_ALIGNED_SIZE) / 2;
	if ((NET_SKB_PAD + I40E_RXBUFFER_1536) <=
	    SKB_WITH_OVERHEAD(*buff_tot_len) && mtu <= ETH_DATA_LEN) {
		*buff_len = I40E_RXBUFFER_1536;
		*headroom = SKB_WITH_OVERHEAD(*buff_tot_len) - *buff_len;

		return;
	}

	*pg_order = 1;
	*buff_tot_len = ((PAGE_SIZE << 1) - I40E_BPRHDR_ALIGNED_SIZE) / 2;
	*buff_len = I40E_RXBUFFER_3072;
	*headroom = SKB_WITH_OVERHEAD(*buff_tot_len) - *buff_len;
}

static void calc_buffer_size_greater_8192(bool reserve_headroom,
					  unsigned int *buff_tot_len,
					  unsigned int *buff_len,
					  unsigned int *headroom,
					  unsigned int *pg_order)
{
	*pg_order = 0;

	if (!reserve_headroom) {
		*buff_tot_len = I40E_RXBUFFER_2048;
		*buff_len = I40E_RXBUFFER_2048;
		*headroom = 0;

		return;
	}

	*buff_tot_len = I40E_RXBUFFER_3072;
	*buff_len = SKB_WITH_OVERHEAD(*buff_tot_len) - NET_SKB_PAD;
	*buff_len = (*buff_len / 128) * 128; /* 128B align */
	*headroom = *buff_tot_len - *buff_len;
}

static void calc_buffer_size(unsigned int mtu, bool reserve_headroom,
			     unsigned int *buff_tot_len,
			     unsigned int *buff_len,
			     unsigned int *headroom,
			     unsigned int *pg_order)
{
	if (PAGE_SIZE < 8192) {
		calc_buffer_size_less_8192(mtu, reserve_headroom,
					   buff_tot_len,
					   buff_len,
					   headroom,
					   pg_order);

		return;
	}

	calc_buffer_size_greater_8192(reserve_headroom, buff_tot_len,
				      buff_len, headroom, pg_order);
}

static void i40e_bpr_destroy(void *pool)
{
	struct i40e_bpr_pool *impl = (struct i40e_bpr_pool *)pool;
	struct i40e_bpr_header *hdr;

	while (impl->head != impl->tail) {
		hdr = impl->buffs[impl->head];
		dma_unmap_page_attrs(impl->dev, hdr->dma, impl->pg_size,
				     DMA_FROM_DEVICE, I40E_RX_DMA_ATTR);
		__page_frag_cache_drain(virt_to_head_page(hdr),
					hdr->pagecnt_bias);
		impl->head = (impl->head + 1) & impl->buffs_size_mask;
	}

	kfree(impl);
}

struct buff_pool *i40e_buff_pool_recycle_create(unsigned int mtu,
						bool reserve_headroom,
						struct device *dev,
						unsigned int pool_size)
{
	struct buff_pool_ops *pool_ops;
	struct i40e_bpr_pool *impl;
	struct buff_pool *pool;

	if (!is_power_of_2(pool_size)) {
		pr_err("%s pool_size (%u) is not power of 2\n", __func__, pool_size);

		return NULL;
	}

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;

	pool_ops = kzalloc(sizeof(*pool_ops), GFP_KERNEL);
	if (!pool_ops) {
		kfree(pool);
		return NULL;
	}

	impl = kzalloc(sizeof(*impl) +
		       pool_size * sizeof(struct i40e_bpr_header *),
		       GFP_KERNEL);
	if (!impl) {
		kfree(pool_ops);
		kfree(pool);
		return NULL;
	}

	calc_buffer_size(mtu, reserve_headroom,
			 &impl->buff_tot_len,
			 &impl->buff_len,
			 &impl->headroom,
			 &impl->pg_order);

	impl->buffs_size_mask = pool_size - 1;
	impl->dev = dev;
	impl->pg_size = PAGE_SIZE << impl->pg_order;

	pool_ops->alloc = i40e_bpr_alloc;
	pool_ops->free = i40e_bpr_free;
	pool_ops->buff_size = i40e_bpr_buff_size;
	pool_ops->total_buff_size = i40e_bpr_total_buff_size;
	pool_ops->buff_headroom = i40e_bpr_buff_headroom;
	pool_ops->buff_truesize = i40e_bpr_buff_truesize;
	pool_ops->buff_ptr = i40e_bpr_buff_ptr;
	pool_ops->buff_convert_to_page = i40e_bpr_buff_convert_to_page;
	pool_ops->buff_dma = i40e_bpr_buff_dma;
	pool_ops->buff_dma_sync_cpu = i40e_bpr_buff_dma_sync_cpu;
	pool_ops->buff_dma_sync_dev = i40e_bpr_buff_dma_sync_dev;
	pool_ops->destroy = i40e_bpr_destroy;

	pool->pool = impl;
	pool->ops = pool_ops;

	return pool;
}


