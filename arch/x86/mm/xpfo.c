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

#include <linux/mm.h>
#include <linux/module.h>

#include <asm/pgtable.h>
#include <asm/tlbflush.h>

#define TEST_XPFO_FLAG(flag, page) \
	test_bit(PG_XPFO_##flag, &(page)->xpfo.flags)

#define SET_XPFO_FLAG(flag, page)			\
	__set_bit(PG_XPFO_##flag, &(page)->xpfo.flags)

#define CLEAR_XPFO_FLAG(flag, page)			\
	__clear_bit(PG_XPFO_##flag, &(page)->xpfo.flags)

#define TEST_AND_CLEAR_XPFO_FLAG(flag, page)			\
	__test_and_clear_bit(PG_XPFO_##flag, &(page)->xpfo.flags)

/*
 * Update a single kernel page table entry
 */
static inline void set_kpte(struct page *page, unsigned long kaddr,
			    pgprot_t prot) {
	unsigned int level;
	pte_t *kpte = lookup_address(kaddr, &level);

	/* We only support 4k pages for now */
	BUG_ON(!kpte || level != PG_LEVEL_4K);

	set_pte_atomic(kpte, pfn_pte(page_to_pfn(page), canon_pgprot(prot)));
}

inline void xpfo_clear_zap(struct page *page, int order)
{
	int i;

	for (i = 0; i < (1 << order); i++)
		CLEAR_XPFO_FLAG(zap, page + i);
}

inline int xpfo_test_and_clear_zap(struct page *page)
{
	return TEST_AND_CLEAR_XPFO_FLAG(zap, page);
}

inline int xpfo_test_kernel(struct page *page)
{
	return TEST_XPFO_FLAG(kernel, page);
}

inline int xpfo_test_user(struct page *page)
{
	return TEST_XPFO_FLAG(user, page);
}

void xpfo_alloc_page(struct page *page, int order, gfp_t gfp)
{
	int i, tlb_shoot = 0;
	unsigned long kaddr;

	for (i = 0; i < (1 << order); i++)  {
		WARN_ON(TEST_XPFO_FLAG(user_fp, page + i) ||
			TEST_XPFO_FLAG(user, page + i));

		if (gfp & GFP_HIGHUSER) {
			/* Initialize the xpfo lock and map counter */
			spin_lock_init(&(page + i)->xpfo.lock);
			atomic_set(&(page + i)->xpfo.mapcount, 0);

			/* Mark it as a user page */
			SET_XPFO_FLAG(user_fp, page + i);

			/*
			 * Shoot the TLB if the page was previously allocated
			 * to kernel space
			 */
			if (TEST_AND_CLEAR_XPFO_FLAG(kernel, page + i))
				tlb_shoot = 1;
		} else {
			/* Mark it as a kernel page */
			SET_XPFO_FLAG(kernel, page + i);
		}
	}

	if (tlb_shoot) {
		kaddr = (unsigned long)page_address(page);
		flush_tlb_kernel_range(kaddr, kaddr + (1 << order) *
				       PAGE_SIZE);
	}
}

void xpfo_free_page(struct page *page, int order)
{
	int i;
	unsigned long kaddr;

	for (i = 0; i < (1 << order); i++) {

		/* The page frame was previously allocated to user space */
		if (TEST_AND_CLEAR_XPFO_FLAG(user, page + i)) {
			kaddr = (unsigned long)page_address(page + i);

			/* Clear the page and mark it accordingly */
			clear_page((void *)kaddr);
			SET_XPFO_FLAG(zap, page + i);

			/* Map it back to kernel space */
			set_kpte(page + i,  kaddr, __pgprot(__PAGE_KERNEL));

			/* No TLB update */
		}

		/* Clear the xpfo fast-path flag */
		CLEAR_XPFO_FLAG(user_fp, page + i);
	}
}

void xpfo_kmap(void *kaddr, struct page *page)
{
	unsigned long flags;

	/* The page is allocated to kernel space, so nothing to do */
	if (TEST_XPFO_FLAG(kernel, page))
		return;

	spin_lock_irqsave(&page->xpfo.lock, flags);

	/*
	 * The page was previously allocated to user space, so map it back
	 * into the kernel. No TLB update required.
	 */
	if ((atomic_inc_return(&page->xpfo.mapcount) == 1) &&
	    TEST_XPFO_FLAG(user, page))
		set_kpte(page, (unsigned long)kaddr, __pgprot(__PAGE_KERNEL));

	spin_unlock_irqrestore(&page->xpfo.lock, flags);
}
EXPORT_SYMBOL(xpfo_kmap);

void xpfo_kunmap(void *kaddr, struct page *page)
{
	unsigned long flags;

	/* The page is allocated to kernel space, so nothing to do */
	if (TEST_XPFO_FLAG(kernel, page))
		return;

	spin_lock_irqsave(&page->xpfo.lock, flags);

	/*
	 * The page frame is to be allocated back to user space. So unmap it
	 * from the kernel, update the TLB and mark it as a user page.
	 */
	if ((atomic_dec_return(&page->xpfo.mapcount) == 0) &&
	    (TEST_XPFO_FLAG(user_fp, page) || TEST_XPFO_FLAG(user, page))) {
		set_kpte(page, (unsigned long)kaddr, __pgprot(0));
		__flush_tlb_one((unsigned long)kaddr);
		SET_XPFO_FLAG(user, page);
	}

	spin_unlock_irqrestore(&page->xpfo.lock, flags);
}
EXPORT_SYMBOL(xpfo_kunmap);
