#include "xsk_buff_pool.h"

#include <linux/skbuff.h>
#include <linux/buff_pool.h>

#include "xsk_packet_array.h" /* XDP_KERNEL_HEADROOM */
#include "xsk_buff.h"
#include "xsk_ring.h"

#define BATCH_SIZE 32

static bool xsk_bp_alloc_from_freelist(struct xsk_buff_pool *impl,
				       unsigned long *handle)
{
	struct xsk_buff *buff;

	if (impl->free_list) {
		buff = impl->free_list;
		impl->free_list = buff->next;
		buff->next = NULL;
		*handle = (unsigned long)buff;

		return true;
	}

	return false;
}

static int xsk_bp_alloc(void *pool, unsigned long *handle)
{
	struct xsk_buff_pool *impl = (struct xsk_buff_pool *)pool;
	struct xsk_buff *buff;
	struct xskq_iter it;
	u32 id;

	if (xsk_bp_alloc_from_freelist(impl, handle))
		return 0;

	it = xskq_deq_iter(impl->q, BATCH_SIZE);

	while (!xskq_iter_end(&it)) {
		id = xskq_deq_iter_get_id(impl->q, &it);
		buff = &impl->bi->buffs[id];
		buff->next = impl->free_list;
		impl->free_list = buff;
		xskq_deq_iter_next(impl->q, &it);
	}

	xskq_deq_iter_done(impl->q, &it);

	if (xsk_bp_alloc_from_freelist(impl, handle))
		return 0;

	return -ENOMEM;
}

static void xsk_bp_free(void *pool, unsigned long handle)
{
	struct xsk_buff_pool *impl = (struct xsk_buff_pool *)pool;
	struct xsk_buff *buff = (struct xsk_buff *)handle;

	buff->next = impl->free_list;
	impl->free_list = buff;
}

static unsigned int xsk_bp_buff_size(void *pool)
{
	struct xsk_buff_pool *impl = (struct xsk_buff_pool *)pool;

	return impl->bi->buff_len - impl->bi->rx_headroom -
		XDP_KERNEL_HEADROOM;
}

static unsigned int xsk_bp_total_buff_size(void *pool)
{
	struct xsk_buff_pool *impl = (struct xsk_buff_pool *)pool;

	return impl->bi->buff_len - impl->bi->rx_headroom;
}

static unsigned int xsk_bp_buff_headroom(void *pool)
{
	(void)pool;

	return XSK_KERNEL_HEADROOM;
}

static unsigned int xsk_bp_buff_truesize(void *pool)
{
	struct xsk_buff_pool *impl = (struct xsk_buff_pool *)pool;

	return impl->bi->buff_len;
}

static void *xsk_bp_buff_ptr(void *pool, unsigned long handle)
{
	struct xsk_buff *buff = (struct xsk_buff *)handle;

	(void)pool;
	return buff->data + buff->offset;
}

static int xsk_bp_buff_convert_to_page(void *pool,
				       unsigned long handle,
				       struct page **pg, unsigned int *pg_off)
{
	unsigned int req_len, buff_len, pg_order = 0;
	void *data;

	buff_len = xsk_bp_total_buff_size(pool);
	req_len = buff_len + SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	/* XXX too sloppy? clean up... */
	if (req_len > PAGE_SIZE) {
		pg_order++;
		if (req_len > (PAGE_SIZE << pg_order))
			return -ENOMEM;
	}

	*pg = dev_alloc_pages(pg_order);
	if (unlikely(!*pg))
		return -ENOMEM;

	data = page_address(*pg);
	memcpy(data, xsk_bp_buff_ptr(pool, handle),
	       xsk_bp_total_buff_size(pool));
	*pg_off = 0;

	xsk_bp_free(pool, handle);

	return 0;
}

static dma_addr_t xsk_bp_buff_dma(void *pool,
				  unsigned long handle)
{
	struct xsk_buff *buff = (struct xsk_buff *)handle;

	(void)pool;
	return buff->dma + buff->offset;
}

static void xsk_bp_buff_dma_sync_cpu(void *pool,
				     unsigned long handle,
				     unsigned int off,
				     unsigned int size)
{
	struct xsk_buff_pool *impl = (struct xsk_buff_pool *)pool;
	struct xsk_buff *buff = (struct xsk_buff *)handle;

	dma_sync_single_range_for_cpu(impl->bi->dev, buff->dma,
				      off, size, impl->bi->dir);
}

static void xsk_bp_buff_dma_sync_dev(void *pool,
				     unsigned long handle,
				     unsigned int off,
				     unsigned int size)
{
	struct xsk_buff_pool *impl = (struct xsk_buff_pool *)pool;
	struct xsk_buff *buff = (struct xsk_buff *)handle;

	dma_sync_single_range_for_device(impl->bi->dev, buff->dma,
					 off, size, impl->bi->dir);
}

static void xsk_bp_destroy(void *pool)
{
	struct xsk_buff_pool *impl = (struct xsk_buff_pool *)pool;
	struct xsk_buff *buff = impl->free_list;

	while (buff) {
		xskq_return_id(impl->q, buff->id);
		buff = buff->next;
	}

	kfree(impl);
}

struct buff_pool *xsk_buff_pool_create(struct xsk_buff_info *buff_info,
				       struct xsk_queue *queue)
{
	struct buff_pool_ops *pool_ops;
	struct xsk_buff_pool *impl;
	struct buff_pool *pool;

	pool = kzalloc(sizeof(*pool), GFP_KERNEL);
	if (!pool)
		return NULL;

	pool_ops = kzalloc(sizeof(*pool_ops), GFP_KERNEL);
	if (!pool_ops) {
		kfree(pool);
		return NULL;
	}

	impl = kzalloc(sizeof(*impl), GFP_KERNEL);
	if (!impl) {
		kfree(pool_ops);
		kfree(pool);
		return NULL;
	}

	impl->bi = buff_info;
	impl->q = queue;

	pool_ops->alloc = xsk_bp_alloc;
	pool_ops->free = xsk_bp_free;
	pool_ops->buff_size = xsk_bp_buff_size;
	pool_ops->total_buff_size = xsk_bp_total_buff_size;
	pool_ops->buff_headroom = xsk_bp_buff_headroom;
	pool_ops->buff_truesize = xsk_bp_buff_truesize;
	pool_ops->buff_ptr = xsk_bp_buff_ptr;
	pool_ops->buff_convert_to_page = xsk_bp_buff_convert_to_page;
	pool_ops->buff_dma = xsk_bp_buff_dma;
	pool_ops->buff_dma_sync_cpu = xsk_bp_buff_dma_sync_cpu;
	pool_ops->buff_dma_sync_dev = xsk_bp_buff_dma_sync_dev;
	pool_ops->destroy = xsk_bp_destroy;

	pool->pool = impl;
	pool->ops = pool_ops;

	return pool;
}

