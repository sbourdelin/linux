/*
 * page_pool.c
 */

/* Using the page pool from a driver, involves
 *
 * 1. Creating/allocating a page_pool per RX ring for the NIC
 * 2. Using pages from page_pool to populate RX ring
 * 3. Page pool will call dma_map/unmap
 * 4. Driver is responsible for dma_sync part
 * 5. On page put/free the page is returned to the page_pool
 *
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>

#include <linux/page_pool.h>
#include <linux/dma-direction.h>
#include <linux/dma-mapping.h>
#include <linux/page-flags.h>
#include <linux/mm.h> /* for __put_page() */
#include "internal.h" /* for set_page_refcounted() */

/*
 * The struct page_pool (likely) cannot be embedded into another
 * structure, because freeing this struct depend on outstanding pages,
 * which can point back to the page_pool. Thus, don't export "init".
 */
static int page_pool_init(struct page_pool *pool,
			  const struct page_pool_params *params)
{
	int ring_qsize = 1024; /* Default */
	int param_copy_sz;

	if (!pool)
		return -EFAULT;

	/* Allow kernel devel trees and driver to progress at different rates */
	param_copy_sz = PAGE_POOL_PARAMS_SIZE;
	memset(&pool->p, 0, param_copy_sz);
	if (params->size < param_copy_sz) {
		/*
		 * Older module calling newer kernel, handled by only
		 * copying supplied size, and keep remaining params zero
		 */
		param_copy_sz = params->size;
	} else if (params->size > param_copy_sz) {
		/*
		 * Newer module calling older kernel. Need to validate
		 * no new features were requested.
		 */
		unsigned char *addr = (unsigned char*)params + param_copy_sz;
		unsigned char *end  = (unsigned char*)params + params->size;

		for (; addr < end; addr++) {
			if (*addr != 0)
				return -E2BIG;
		}
	}
	memcpy(&pool->p, params, param_copy_sz);

	/* Validate only known flags were used */
	if (pool->p.flags & ~(PP_FLAG_ALL))
		return -EINVAL;

	if (pool->p.pool_size)
		ring_qsize = pool->p.pool_size;

	/* ptr_ring is not meant as final struct, see page_pool.h */
	if (ptr_ring_init(&pool->ring, ring_qsize, GFP_KERNEL) < 0) {
		return -ENOMEM;
	}

	/*
	 * DMA direction is either DMA_FROM_DEVICE or DMA_BIDIRECTIONAL.
	 * DMA_BIDIRECTIONAL is for allowing page used for DMA sending,
	 * which is the XDP_TX use-case.
	 */
	if ((pool->p.dma_dir != DMA_FROM_DEVICE) &&
	    (pool->p.dma_dir != DMA_BIDIRECTIONAL))
		return -EINVAL;

	return 0;
}

struct page_pool *page_pool_create(const struct page_pool_params *params)
{
	struct page_pool *pool;
	int err = 0;

	if (params->size < offsetof(struct page_pool_params, nid)) {
		WARN(1, "Fix page_pool_params->size code\n");
		return NULL;
	}

	pool = kzalloc_node(sizeof(*pool), GFP_KERNEL, params->nid);
	err = page_pool_init(pool, params);
	if (err < 0) {
		pr_warn("%s() gave up with errno %d\n", __func__, err);
		kfree(pool);
		return ERR_PTR(err);
	}
	return pool;
}
EXPORT_SYMBOL(page_pool_create);

/* fast path */
static struct page *__page_pool_get_cached(struct page_pool *pool)
{
	struct ptr_ring *r;
	struct page *page;

	/* Caller guarantee safe context for accessing alloc.cache */
	if (likely(pool->alloc.count)) {
		/* Fast-path */
		page = pool->alloc.cache[--pool->alloc.count];
		return page;
	}

	/* Slower-path: Alloc array empty, time to refill */
	r = &pool->ring;
	/* Open-coded bulk ptr_ring consumer.
	 *
	 * Discussion: ATM ring *consumer* lock is not really needed
	 * due to caller protecton, but later MM-layer need the
	 * ability to reclaim pages from ring. Thus, keeping locks.
	 */
	spin_lock(&r->consumer_lock);
	while ((page = __ptr_ring_consume(r))) {
		/* Pages on ring refcnt==0, on alloc.cache refcnt==1 */
		set_page_refcounted(page);
		if (pool->alloc.count == PP_ALLOC_CACHE_REFILL)
			break;
		pool->alloc.cache[pool->alloc.count++] = page;
	}
	spin_unlock(&r->consumer_lock);
	return page;
}

/* slow path */
noinline
static struct page *__page_pool_alloc_pages(struct page_pool *pool,
					    gfp_t _gfp)
{
	struct page *page;
	gfp_t gfp = _gfp;
	dma_addr_t dma;

	/* We could always set __GFP_COMP, and avoid this branch, as
	 * prep_new_page() can handle order-0 with __GFP_COMP.
	 */
	if (pool->p.order)
		gfp |= __GFP_COMP;
	/*
	 *  Discuss GFP flags: e.g
	 *   __GFP_NOWARN + __GFP_NORETRY + __GFP_NOMEMALLOC
	 */

	/*
	 * FUTURE development:
	 *
	 * Current slow-path essentially falls back to single page
	 * allocations, which doesn't improve performance.  This code
	 * need bulk allocation support from the page allocator code.
	 *
	 * For now, page pool recycle cache is not refilled.  Hint:
	 * when pages are returned, they will go into the recycle
	 * cache.
	 */

	/* Cache was empty, do real allocation */
	page = alloc_pages_node(pool->p.nid, gfp, pool->p.order);
	if (!page)
		return NULL;

	/* FIXME: Add accounting of pages.
	 *
	 * TODO: Look into memcg_charge_slab/memcg_uncharge_slab
	 *
	 * What if page comes from pfmemalloc reserves?
	 * Should we abort to help memory pressure? (test err code path!)
	 * Code see SetPageSlabPfmemalloc(), __ClearPageSlabPfmemalloc()
	 * and page_is_pfmemalloc(page)
	 */

	/* Setup DMA mapping:
	 * This mapping is kept for lifetime of page, until leaving pool.
	 */
	dma = dma_map_page(pool->p.dev, page, 0,
			   (PAGE_SIZE << pool->p.order),
			   pool->p.dma_dir);
	if (dma_mapping_error(pool->p.dev, dma)) {
		put_page(page);
		return NULL;
	}
	page->dma_addr = dma;

	/* IDEA: When page just alloc'ed is should/must have refcnt 1.
	 * Should we do refcnt inc tricks to keep page mapped/owned by
	 * page_pool infrastructure? (like page_frag code)
	 */

	/* TODO: Init fields in struct page. See slub code allocate_slab()
	 *
	 */
	page->pool = pool;   /* Save pool the page MUST be returned to */
	__SetPagePool(page); /* Mark page with flag */

	return page;
}


/* For using page_pool replace: alloc_pages() API calls, but provide
 * synchronization guarantee for allocation side.
 */
struct page *page_pool_alloc_pages(struct page_pool *pool, gfp_t gfp)
{
	struct page *page;

	/* Fast-path: Get a page from cache */
	page = __page_pool_get_cached(pool);
	if (page)
		return page;

	/* Slow-path: cache empty, do real allocation */
	page = __page_pool_alloc_pages(pool, gfp);
	return page;
}
EXPORT_SYMBOL(page_pool_alloc_pages);

/* Cleanup page_pool state from page */
// Ideas taken from __free_slab()
static void __page_pool_clean_page(struct page *page)
{
	struct page_pool *pool;

	VM_BUG_ON_PAGE(!PagePool(page), page);

	// mod_zone_page_state() ???

	pool = page->pool;
	__ClearPagePool(page);

	/* DMA unmap */
	dma_unmap_page(pool->p.dev, page->dma_addr,
		       PAGE_SIZE << pool->p.order,
                       pool->p.dma_dir);
	page->dma_addr = 0;
        /* Q: Use DMA macros???
	 *
	 * dma_unmap_page(pool->p.dev, dma_unmap_addr(page,dma_addr),
	 *	       PAGE_SIZE << pool->p.order,
	 *	       pool->p.dma_dir);
	 * dma_unmap_addr_set(page, dma_addr, 0);
	 */

	/* FUTURE: Use Alex Duyck's DMA_ATTR_SKIP_CPU_SYNC changes
	 *
	 * dma_unmap_page_attrs(pool->p.dev, page->dma_addr,
	 *		     PAGE_SIZE << pool->p.order,
	 *		     pool->p.dma_dir,
	 *		     DMA_ATTR_SKIP_CPU_SYNC);
	 */

	// page_mapcount_reset(page); // ??
	// page->mapping = NULL;      // ??

	// Not really needed, but good for provoking bugs
	page->pool = (void *)0xDEADBEE0;

	/* FIXME: Add accounting of pages here!
	 *
	 * Look into: memcg_uncharge_page_pool(page, order, pool);
	 */

	// FIXME: do we need this??? likely not as slub does not...
//	if (unlikely(is_zone_device_page(page)))
//		put_zone_device_page(page);

}

/* Return a page to the page allocator, cleaning up our state */
static void __page_pool_return_page(struct page *page)
{
	VM_BUG_ON_PAGE(page_ref_count(page) != 0, page);
	__page_pool_clean_page(page);
	__put_page(page);
}

bool __page_pool_recycle_into_ring(struct page_pool *pool,
				   struct page *page)
{
	int ret;
	/* TODO: Use smarter data structure for recycle cache.  Using
	 * ptr_ring will not scale when multiple remote CPUs want to
	 * recycle pages.
	 */

	/* Need BH protection when free occurs from userspace e.g
	 * __kfree_skb() called via {tcp,inet,sock}_recvmsg
	 *
	 * Problematic for several reasons: (1) it is more costly,
	 * (2) the BH unlock can cause (re)sched of softirq.
	 *
	 * BH protection not needed if current is serving softirq
	 */
	if (in_serving_softirq())
		ret = ptr_ring_produce(&pool->ring, page);
	else
		ret = ptr_ring_produce_bh(&pool->ring, page);

	return (ret == 0) ? true : false;
}

/*
 * Only allow direct recycling in very special circumstances, into the
 * alloc cache.  E.g. XDP_DROP use-case.
 *
 * Caller must provide appropiate safe context.
 */
// noinline /* hack for perf-record test */
static
bool __page_pool_recycle_direct(struct page *page,
				       struct page_pool *pool)
{
	VM_BUG_ON_PAGE(page_ref_count(page) != 1, page);
	/* page refcnt==1 invarians on alloc.cache */

	if (unlikely(pool->alloc.count == PP_ALLOC_CACHE_SIZE))
		return false;

	pool->alloc.cache[pool->alloc.count++] = page;
	return true;
}

/*
 * Called when refcnt reach zero.  On failure page_pool state is
 * cleared, and caller can return page to page allocator.
 */
bool page_pool_recycle(struct page *page)
{
	struct page_pool *pool = page->pool;

	VM_BUG_ON_PAGE(page_ref_count(page) != 0, page);

	/* Pages on recycle ring have refcnt==0 */
	if (!__page_pool_recycle_into_ring(pool, page)) {
		__page_pool_clean_page(page);
		return false;
	}
	return true;
}
EXPORT_SYMBOL(page_pool_recycle);

void __page_pool_put_page(struct page *page, bool allow_direct)
{
	struct page_pool *pool = page->pool;

	if (allow_direct && (page_ref_count(page) == 1))
		if (__page_pool_recycle_direct(page, pool))
			return;

	if (put_page_testzero(page))
		if (!page_pool_recycle(page))
			__put_page(page);

}
EXPORT_SYMBOL(__page_pool_put_page);

void __destructor_return_page(void *ptr)
{
	struct page *page = ptr;

	/* Verify the refcnt invariant of cached pages */
	if (page_ref_count(page) != 0) {
		pr_crit("%s() page_pool refcnt %d violation\n",
			__func__, page_ref_count(page));
		BUG();
	}
	__page_pool_return_page(page);
}

/* Cleanup and release resources */
void page_pool_destroy(struct page_pool *pool)
{
	/* Empty recycle ring */
	ptr_ring_cleanup(&pool->ring, __destructor_return_page);

	/* FIXME-mem-leak: cleanup array/stack cache
	 * pool->alloc. Driver usually will destroy RX ring after
	 * making sure nobody can alloc from it, thus it should be
	 * safe to just empty cache here
	 */

	/* FIXME: before releasing the page_pool memory, we MUST make
	 * sure no pages points back this page_pool.
	 */
	kfree(pool);
}
EXPORT_SYMBOL(page_pool_destroy);
