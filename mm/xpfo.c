/*
 * Copyright (C) 2016 Hewlett Packard Enterprise Development, L.P.
 * Copyright (C) 2016 Brown University. All rights reserved.
 *
 * Authors:
 *   Juerg Haefliger <juerg.haefliger@hpe.com>
 *   Vasileios P. Kemerlis <vpk@cs.brown.edu>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/page_ext.h>
#include <linux/xpfo.h>

#include <asm/tlbflush.h>

DEFINE_STATIC_KEY_FALSE(xpfo_inited);

static bool need_xpfo(void)
{
	return true;
}

static void init_xpfo(void)
{
	printk(KERN_INFO "XPFO enabled\n");
	static_branch_enable(&xpfo_inited);
}

struct page_ext_operations page_xpfo_ops = {
	.need = need_xpfo,
	.init = init_xpfo,
};

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

void xpfo_alloc_page(struct page *page, int order, gfp_t gfp)
{
	int i, flush_tlb = 0;
	struct page_ext *page_ext;
	unsigned long kaddr;

	if (!static_branch_unlikely(&xpfo_inited))
		return;

	for (i = 0; i < (1 << order); i++)  {
		page_ext = lookup_page_ext(page + i);

		BUG_ON(test_bit(PAGE_EXT_XPFO_UNMAPPED, &page_ext->flags));

		/* Initialize the map lock and map counter */
		if (!page_ext->inited) {
			spin_lock_init(&page_ext->maplock);
			atomic_set(&page_ext->mapcount, 0);
			page_ext->inited = 1;
		}
		BUG_ON(atomic_read(&page_ext->mapcount));

		if ((gfp & GFP_HIGHUSER) == GFP_HIGHUSER) {
			/*
			 * Flush the TLB if the page was previously allocated
			 * to the kernel.
			 */
			if (test_and_clear_bit(PAGE_EXT_XPFO_KERNEL,
					       &page_ext->flags))
				flush_tlb = 1;
		} else {
			/* Tag the page as a kernel page */
			set_bit(PAGE_EXT_XPFO_KERNEL, &page_ext->flags);
		}
	}

	if (flush_tlb) {
		kaddr = (unsigned long)page_address(page);
		flush_tlb_kernel_range(kaddr, kaddr + (1 << order) *
				       PAGE_SIZE);
	}
}

void xpfo_free_page(struct page *page, int order)
{
	int i;
	struct page_ext *page_ext;
	unsigned long kaddr;

	if (!static_branch_unlikely(&xpfo_inited))
		return;

	for (i = 0; i < (1 << order); i++) {
		page_ext = lookup_page_ext(page + i);

		if (!page_ext->inited) {
			/*
			 * The page was allocated before page_ext was
			 * initialized, so it is a kernel page and it needs to
			 * be tagged accordingly.
			 */
			set_bit(PAGE_EXT_XPFO_KERNEL, &page_ext->flags);
			continue;
		}

		/*
		 * Map the page back into the kernel if it was previously
		 * allocated to user space.
		 */
		if (test_and_clear_bit(PAGE_EXT_XPFO_UNMAPPED,
				       &page_ext->flags)) {
			kaddr = (unsigned long)page_address(page + i);
			set_kpte(page + i,  kaddr, __pgprot(__PAGE_KERNEL));
		}
	}
}

void xpfo_kmap(void *kaddr, struct page *page)
{
	struct page_ext *page_ext;
	unsigned long flags;

	if (!static_branch_unlikely(&xpfo_inited))
		return;

	page_ext = lookup_page_ext(page);

	/*
	 * The page was allocated before page_ext was initialized (which means
	 * it's a kernel page) or it's allocated to the kernel, so nothing to
	 * do.
	 */
	if (!page_ext->inited ||
	    test_bit(PAGE_EXT_XPFO_KERNEL, &page_ext->flags))
		return;

	spin_lock_irqsave(&page_ext->maplock, flags);

	/*
	 * The page was previously allocated to user space, so map it back
	 * into the kernel. No TLB flush required.
	 */
	if ((atomic_inc_return(&page_ext->mapcount) == 1) &&
	    test_and_clear_bit(PAGE_EXT_XPFO_UNMAPPED, &page_ext->flags))
		set_kpte(page, (unsigned long)kaddr, __pgprot(__PAGE_KERNEL));

	spin_unlock_irqrestore(&page_ext->maplock, flags);
}
EXPORT_SYMBOL(xpfo_kmap);

void xpfo_kunmap(void *kaddr, struct page *page)
{
	struct page_ext *page_ext;
	unsigned long flags;

	if (!static_branch_unlikely(&xpfo_inited))
		return;

	page_ext = lookup_page_ext(page);

	/*
	 * The page was allocated before page_ext was initialized (which means
	 * it's a kernel page) or it's allocated to the kernel, so nothing to
	 * do.
	 */
	if (!page_ext->inited ||
	    test_bit(PAGE_EXT_XPFO_KERNEL, &page_ext->flags))
		return;

	spin_lock_irqsave(&page_ext->maplock, flags);

	/*
	 * The page is to be allocated back to user space, so unmap it from the
	 * kernel, flush the TLB and tag it as a user page.
	 */
	if (atomic_dec_return(&page_ext->mapcount) == 0) {
		BUG_ON(test_bit(PAGE_EXT_XPFO_UNMAPPED, &page_ext->flags));
		set_bit(PAGE_EXT_XPFO_UNMAPPED, &page_ext->flags);
		set_kpte(page, (unsigned long)kaddr, __pgprot(0));
		__flush_tlb_one((unsigned long)kaddr);
	}

	spin_unlock_irqrestore(&page_ext->maplock, flags);
}
EXPORT_SYMBOL(xpfo_kunmap);

inline bool xpfo_page_is_unmapped(struct page *page)
{
	if (!static_branch_unlikely(&xpfo_inited))
		return false;

	return test_bit(PAGE_EXT_XPFO_UNMAPPED, &lookup_page_ext(page)->flags);
}
