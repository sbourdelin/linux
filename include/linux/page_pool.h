/*
 * page_pool.h
 *
 *	Author:	Jesper Dangaard Brouer <netoptimizer@brouer.com>
 *	Copyright (C) 2016 Red Hat, Inc.
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation; either version 2 of the License, or (at your
 *	option) any later version.
 *
 * The page_pool is primarily motivated by two things (1) performance
 * and (2) changing the memory model for drivers.
 *
 * Drivers have developed performance workarounds when the speed of
 * the page allocator and the DMA APIs became too slow for their HW
 * needs. The page pool solves them on a general level providing
 * performance gains and benefits that local driver recycling hacks
 * cannot realize.
 *
 * A fundamental property is that pages are returned to the page_pool.
 * This property allow a certain class of optimizations, which is to
 * move setup and tear-down operations out of the fast-path, sometimes
 * known as constructor/destruction operations.  DMA map/unmap is one
 * example of operations this applies to.  Certain page alloc/free
 * validations can also be avoided in the fast-path.  Another example
 * could be pre-mapping pages into userspace, and clearing them
 * (memset-zero) outside the fast-path.
 *
 * This API is only meant for streaming DMA, which map/unmap frequently.
 */
#ifndef _LINUX_PAGE_POOL_H
#define _LINUX_PAGE_POOL_H

/*
 * NOTES on page flags (PG_pool)... we might have a problem with
 * enough page flags on 32 bit systems, example see PG_idle + PG_young
 * include/linux/page_idle.h and CONFIG_IDLE_PAGE_TRACKING
 */

#include <linux/ptr_ring.h>

//#include <linux/dma-mapping.h>
#include <linux/dma-direction.h>

// Not-used-atm #define PP_FLAG_NAPI 0x1
#define PP_FLAG_ALL	0

/*
 * Fast allocation side cache array/stack
 *
 * The cache size and refill watermark is related to the network
 * use-case.  The NAPI budget is 64 packets.  After a NAPI poll the RX
 * ring is usually refilled and the max consumed elements will be 64,
 * thus a natural max size of objects needed in the cache.
 *
 * Keeping room for more objects, is due to XDP_DROP use-case.  As
 * XDP_DROP allows the opportunity to recycle objects directly into
 * this array, as it shares the same softirq/NAPI protection.  If
 * cache is already full (or partly full) then the XDP_DROP recycles
 * would have to take a slower code path.
 */
#define PP_ALLOC_CACHE_SIZE	128
#define PP_ALLOC_CACHE_REFILL	64
struct pp_alloc_cache {
	u32 count ____cacheline_aligned_in_smp;
	u32 refill; /* not used atm */
	void *cache[PP_ALLOC_CACHE_SIZE];
};

/*
 * Extensible params struct. Focus on currently implemented features,
 * extend later. Restriction, subsequently added members value of zero
 * must gives the previous behaviour. Avoids need to update every
 * driver simultaniously (given likely in difference subsystems).
 */
struct page_pool_params {
	u32		size; /* caller sets size of struct */
	unsigned int	order;
	unsigned long	flags;
	/* Associated with a specific device, for DMA pre-mapping purposes */
	struct device	*dev;
	/* Numa node id to allocate from pages from */
	int 		nid;
	enum dma_data_direction dma_dir; /* DMA mapping direction */
	unsigned int	pool_size;
	char		end_marker[0]; /* must be last struct member */
};
#define	PAGE_POOL_PARAMS_SIZE	offsetof(struct page_pool_params, end_marker)

struct page_pool {
	struct page_pool_params p;

	/*
	 * Data structure for allocation side
	 *
	 * Drivers allocation side usually already perform some kind
	 * of resource protection.  Piggyback on this protection, and
	 * require driver to protect allocation side.
	 *
	 * For NIC drivers this means, allocate a page_pool per
	 * RX-queue. As the RX-queue is already protected by
	 * Softirq/BH scheduling and napi_schedule. NAPI schedule
	 * guarantee that a single napi_struct will only be scheduled
	 * on a single CPU (see napi_schedule).
	 */
	struct pp_alloc_cache alloc;

	/* Data structure for storing recycled pages.
	 *
	 * Returning/freeing pages is more complicated synchronization
	 * wise, because free's can happen on remote CPUs, with no
	 * association with allocation resource.
	 *
	 * XXX: Mel says drop comment
	 * For now use ptr_ring, as it separates consumer and
	 * producer, which is a common use-case. The ptr_ring is not
	 * though as the final data structure, expecting this to
	 * change into a more advanced data structure with more
	 * integration with page_alloc.c and data structs per CPU for
	 * returning pages in bulk.
	 *
	 */
	struct ptr_ring ring;

	/* TODO: Domain "id" add later, for RX zero-copy validation */

	/* TODO: Need list pointers for keeping page_pool object on a
	 * cleanup list, given pages can be "outstanding" even after
	 * e.g. driver is unloaded.
	 */
};

struct page* page_pool_alloc_pages(struct page_pool *pool, gfp_t gfp);

static inline struct page *page_pool_dev_alloc_pages(struct page_pool *pool)
{
	gfp_t gfp = (GFP_ATOMIC | __GFP_NOWARN | __GFP_COLD);
	return page_pool_alloc_pages(pool, gfp);
}

struct page_pool *page_pool_create(const struct page_pool_params *params);

void page_pool_destroy(struct page_pool *pool);

/* Never call this directly, use helpers below */
void __page_pool_put_page(struct page *page, bool allow_direct);

/* XXX: Mel: needs descriptions*/
static inline void page_pool_put_page(struct page *page)
{
	__page_pool_put_page(page, false);
}
/* Very limited use-cases allow recycle direct */
static inline void page_pool_recycle_direct(struct page *page)
{
	__page_pool_put_page(page, true);
}

/*
 * Called when refcnt reach zero.  On failure page_pool state is
 * cleared, and caller can return page to page allocator.
 */
bool page_pool_recycle(struct page *page);
// XXX: compile out trick, let this return false compile time,
// or let PagePool() check compile to false.

#endif /* _LINUX_PAGE_POOL_H */
