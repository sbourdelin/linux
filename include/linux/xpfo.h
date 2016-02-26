/*
 * Copyright (C) 2016 Brown University. All rights reserved.
 * Copyright (C) 2016 Hewlett Packard Enterprise Development, L.P.
 *
 * Authors:
 *   Vasileios P. Kemerlis <vpk@cs.brown.edu>
 *   Juerg Haefliger <juerg.haefliger@hpe.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#ifndef _LINUX_XPFO_H
#define _LINUX_XPFO_H

#ifdef CONFIG_XPFO

/*
 * XPFO page flags:
 *
 * PG_XPFO_user_fp denotes that the page is allocated to user space. This flag
 * is used in the fast path, where the page is marked accordingly but *not*
 * unmapped from the kernel. In most cases, the kernel will need access to the
 * page immediately after its acquisition so an unnecessary mapping operation
 * is avoided.
 *
 * PG_XPFO_user denotes that the page is destined for user space. This flag is
 * used in the slow path, where the page needs to be mapped/unmapped when the
 * kernel wants to access it. If a page is deallocated and this flag is set,
 * the page is cleared and mapped back into the kernel.
 *
 * PG_XPFO_kernel denotes a page that is destined to kernel space. This is used
 * for identifying pages that are first assigned to kernel space and then freed
 * and mapped to user space. In such cases, an expensive TLB shootdown is
 * necessary. Pages allocated to user space, freed, and subsequently allocated
 * to user space again, require only local TLB invalidation.
 *
 * PG_XPFO_zap indicates that the page has been zapped. This flag is used to
 * avoid zapping pages multiple times. Whenever a page is freed and was
 * previously mapped to user space, it needs to be zapped before mapped back
 * in to the kernel.
 */

enum xpfo_pageflags {
	PG_XPFO_user_fp,
	PG_XPFO_user,
	PG_XPFO_kernel,
	PG_XPFO_zap,
};

struct xpfo_info {
	unsigned long flags;	/* Flags for tracking the page's XPFO state */
	atomic_t mapcount;	/* Counter for balancing page map/unmap
				 * requests. Only the first map request maps
				 * the page back to kernel space. Likewise,
				 * only the last unmap request unmaps the page.
				 */
	spinlock_t lock;	/* Lock to serialize concurrent map/unmap
				 * requests.
				 */
};

extern void xpfo_clear_zap(struct page *page, int order);
extern int xpfo_test_and_clear_zap(struct page *page);
extern int xpfo_test_kernel(struct page *page);
extern int xpfo_test_user(struct page *page);

extern void xpfo_kmap(void *kaddr, struct page *page);
extern void xpfo_kunmap(void *kaddr, struct page *page);
extern void xpfo_alloc_page(struct page *page, int order, gfp_t gfp);
extern void xpfo_free_page(struct page *page, int order);

#else /* ifdef CONFIG_XPFO */

static inline void xpfo_clear_zap(struct page *page, int order) { }
static inline int xpfo_test_and_clear_zap(struct page *page) { return 0; }
static inline int xpfo_test_kernel(struct page *page) { return 0; }
static inline int xpfo_test_user(struct page *page) { return 0; }

static inline void xpfo_kmap(void *kaddr, struct page *page) { }
static inline void xpfo_kunmap(void *kaddr, struct page *page) { }
static inline void xpfo_alloc_page(struct page *page, int order, gfp_t gfp) { }
static inline void xpfo_free_page(struct page *page, int order) { }

#endif /* ifdef CONFIG_XPFO */

#endif /* ifndef _LINUX_XPFO_H */
