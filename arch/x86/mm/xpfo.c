/*
 * Copyright (C) 2017 Hewlett Packard Enterprise Development, L.P.
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

#include <asm/tlbflush.h>

/* Update a single kernel page table entry */
inline void set_kpte(void *kaddr, struct page *page, pgprot_t prot)
{
	unsigned int level;
	pte_t *pte = lookup_address((unsigned long)kaddr, &level);


	BUG_ON(!pte);

	switch (level) {
	case PG_LEVEL_4K:
		set_pte_atomic(pte, pfn_pte(page_to_pfn(page), canon_pgprot(prot)));
		break;
	case PG_LEVEL_2M:
	case PG_LEVEL_1G: {
		struct cpa_data cpa;
		int do_split;

		memset(&cpa, 0, sizeof(cpa));
		cpa.vaddr = kaddr;
		cpa.pages = &page;
		cpa.mask_set = prot;
		pgprot_val(cpa.mask_clr) = ~pgprot_val(prot);
		cpa.numpages = 1;
		cpa.flags = 0;
		cpa.curpage = 0;
		cpa.force_split = 0;

		do_split = try_preserve_large_page(pte, (unsigned long)kaddr, &cpa);
		if (do_split < 0)
			BUG_ON(split_large_page(&cpa, pte, (unsigned long)kaddr));

		break;
	}
	default:
		BUG();
	}

}

inline void xpfo_flush_kernel_page(struct page *page, int order)
{
	int level;
	unsigned long size, kaddr;

	kaddr = (unsigned long)page_address(page);
	lookup_address(kaddr, &level);


	switch (level) {
	case PG_LEVEL_4K:
		size = PAGE_SIZE;
		break;
	case PG_LEVEL_2M:
		size = PMD_SIZE;
		break;
	case PG_LEVEL_1G:
		size = PUD_SIZE;
		break;
	default:
		BUG();
	}

	flush_tlb_kernel_range(kaddr, kaddr + (1 << order) * size);
}
