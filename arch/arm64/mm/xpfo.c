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

#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/xpfo.h>

#include <asm/tlbflush.h>

/*
 * Lookup the page table entry for a virtual address and return a pointer to
 * the entry. Based on x86 tree.
 */
static pte_t *lookup_address(unsigned long addr)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset_k(addr);
	if (pgd_none(*pgd))
		return NULL;

	BUG_ON(pgd_bad(*pgd));

	pud = pud_offset(pgd, addr);
	if (pud_none(*pud))
		return NULL;

	BUG_ON(pud_bad(*pud));

	pmd = pmd_offset(pud, addr);
	if (pmd_none(*pmd))
		return NULL;

	BUG_ON(pmd_bad(*pmd));

	return pte_offset_kernel(pmd, addr);
}

/* Update a single kernel page table entry */
inline void set_kpte(void *kaddr, struct page *page, pgprot_t prot)
{
	pte_t *pte = lookup_address((unsigned long)kaddr);

	set_pte(pte, pfn_pte(page_to_pfn(page), prot));
}

inline void xpfo_flush_kernel_page(struct page *page, int order)
{
	unsigned long kaddr = (unsigned long)page_address(page);
	unsigned long size = PAGE_SIZE;

	flush_tlb_kernel_range(kaddr, kaddr + (1 << order) * size);
}

inline void xpfo_dma_map_unmap_area(bool map, const void *addr, size_t size,
				    int dir)
{
	unsigned long flags;
	struct page *page = virt_to_page(addr);

	/*
	 * +2 here because we really want
	 * ceil(size / PAGE_SIZE), not floor(), and one extra in case things are
	 * not page aligned
	 */
	int i, possible_pages = size / PAGE_SIZE + 2;
	void *buf[possible_pages];

	memset(buf, 0, sizeof(void *) * possible_pages);

	local_irq_save(flags);

	/* Map the first page */
	if (xpfo_page_is_unmapped(page))
		buf[0] = kmap_atomic(page);

	/* Map the remaining pages */
	for (i = 1; i < possible_pages; i++) {
		if (page_to_virt(page + i) >= addr + size)
			break;

		if (xpfo_page_is_unmapped(page + i))
			buf[i] = kmap_atomic(page + i);
	}

	if (map)
		__dma_map_area(addr, size, dir);
	else
		__dma_unmap_area(addr, size, dir);

	for (i = 0; i < possible_pages; i++)
		if (buf[i])
			kunmap_atomic(buf[i]);

	local_irq_restore(flags);
}
