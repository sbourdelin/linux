/*
 *  Based on arch/arm/mm/dma-mapping.c which is
 *  Copyright (C) 2000-2004 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/highmem.h>

#include <asm/highmem.h>
#include <asm/cacheflush.h>

static void __force_cache_op(struct page *page, size_t size,
				void (*op)(void *start, size_t size))
{
	unsigned long pfn;
	size_t left = size;

	pfn = page_to_pfn(page);

	do {
		size_t len = left;
		void *vaddr;

		page = pfn_to_page(pfn);

		if (PageHighMem(page)) {
			if (len > PAGE_SIZE)
				len = PAGE_SIZE;
			if (cache_is_vipt_nonaliasing()) {
				vaddr = kmap_atomic(page);
				op(vaddr, len);
				kunmap_atomic(vaddr);
			} else {
				vaddr = kmap_high_get(page);
				if (vaddr) {
					op(vaddr, len);
					kunmap_high(page);
				}
			}
		} else {

			op(page_address(page), len);
		}
		pfn++;
		left -= len;
	} while(left);
}

void kernel_force_cache_clean(struct page *page, size_t size)
{
	phys_addr_t paddr;

	paddr = page_to_phys(page);
	__force_cache_op(page, size, __cpuc_force_dcache_clean);
	outer_clean_range(paddr, paddr + size);
}

void kernel_force_cache_invalidate(struct page *page, size_t size)
{
	phys_addr_t paddr;

	paddr = page_to_phys(page);
	__force_cache_op(page, size, __cpuc_force_dcache_invalidate);
	outer_inv_range(paddr, paddr + size);
}
