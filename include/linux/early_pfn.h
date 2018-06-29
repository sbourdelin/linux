/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (C) 2018 HXT-semitech Corp. */
#ifndef __EARLY_PFN_H
#define __EARLY_PFN_H
#ifdef CONFIG_HAVE_MEMBLOCK_PFN_VALID
ulong __init_memblock memblock_next_valid_pfn(ulong pfn)
{
	struct memblock_type *type = &memblock.memory;
	unsigned int right = type->cnt;
	unsigned int mid, left = 0;
	phys_addr_t addr = PFN_PHYS(++pfn);

	do {
		mid = (right + left) / 2;

		if (addr < type->regions[mid].base)
			right = mid;
		else if (addr >= (type->regions[mid].base +
				  type->regions[mid].size))
			left = mid + 1;
		else {
			/* addr is within the region, so pfn is valid */
			return pfn;
		}
	} while (left < right);

	if (right == type->cnt)
		return -1UL;
	else
		return PHYS_PFN(type->regions[right].base);
}
EXPORT_SYMBOL(memblock_next_valid_pfn);
#endif /*CONFIG_HAVE_MEMBLOCK_PFN_VALID*/
#endif /*__EARLY_PFN_H*/
