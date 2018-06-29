/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018 HXT-semitech Corp. */
#ifndef __EARLY_PFN_H
#define __EARLY_PFN_H
#ifdef CONFIG_HAVE_MEMBLOCK_PFN_VALID
static int early_region_idx __init_memblock = -1;
ulong __init_memblock memblock_next_valid_pfn(ulong pfn)
{
	struct memblock_type *type = &memblock.memory;
	struct memblock_region *regions = type->regions;
	uint right = type->cnt;
	uint mid, left = 0;
	ulong start_pfn, end_pfn, next_start_pfn;
	phys_addr_t addr = PFN_PHYS(++pfn);

	/* fast path, return pfn+1 if next pfn is in the same region */
	if (early_region_idx != -1) {
		start_pfn = PFN_DOWN(regions[early_region_idx].base);
		end_pfn = PFN_DOWN(regions[early_region_idx].base +
				regions[early_region_idx].size);

		if (pfn >= start_pfn && pfn < end_pfn)
			return pfn;

		early_region_idx++;
		next_start_pfn = PFN_DOWN(regions[early_region_idx].base);

		if (pfn >= end_pfn && pfn <= next_start_pfn)
			return next_start_pfn;
	}

	/* slow path, do the binary searching */
	do {
		mid = (right + left) / 2;

		if (addr < regions[mid].base)
			right = mid;
		else if (addr >= (regions[mid].base + regions[mid].size))
			left = mid + 1;
		else {
			early_region_idx = mid;
			return pfn;
		}
	} while (left < right);

	if (right == type->cnt)
		return -1UL;

	early_region_idx = right;

	return PHYS_PFN(regions[early_region_idx].base);
}
EXPORT_SYMBOL(memblock_next_valid_pfn);
#endif /*CONFIG_HAVE_MEMBLOCK_PFN_VALID*/
#endif /*__EARLY_PFN_H*/
