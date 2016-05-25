/*
 * Copyright (C) 2016 Laura Abbott <laura@labbott.name>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/types.h>
#include <linux/scatterlist.h>
#include <linux/dma-mapping.h>
#include <linux/highmem.h>

#include <asm/cacheflush.h>

#include "ion_priv.h"


void ion_clean_page(struct page *page, size_t size)
{
	unsigned long pfn;
	size_t left = size;

	pfn = page_to_pfn(page);

	/*
	 * A single sg entry may refer to multiple physically contiguous
	 * pages.  But we still need to process highmem pages individually.
	 * If highmem is not configured then the bulk of this loop gets
	 * optimized out.
	 */
	do {
		size_t len = left;
		void *vaddr;

		page = pfn_to_page(pfn);

		if (PageHighMem(page)) {
			if (len > PAGE_SIZE)
				len = PAGE_SIZE;

			if (cache_is_vipt_nonaliasing()) {
				vaddr = kmap_atomic(page);
				__cpuc_flush_dcache_area(vaddr, len);
				kunmap_atomic(vaddr);
			} else {
				vaddr = kmap_high_get(page);
				if (vaddr) {
					__cpuc_flush_dcache_area(vaddr, len);
					kunmap_high(page);
				}
			}
		} else {
			vaddr = page_address(page);
			__cpuc_flush_dcache_area(vaddr, len);
		}
		pfn++;
		left -= len;
	} while (left);
}

/*
 * ARM has highmem and a bunch of other 'fun' features. It's so much easier just
 * to do the ISA DMA and call things that way
 */

void ion_invalidate_buffer(struct ion_buffer *buffer)
{
	dma_unmap_sg(NULL, buffer->sg_table->sgl, buffer->sg_table->orig_nents,
			DMA_BIDIRECTIONAL);
}

void ion_clean_buffer(struct ion_buffer *buffer)
{
	dma_map_sg(NULL, buffer->sg_table->sgl, buffer->sg_table->orig_nents,
			DMA_BIDIRECTIONAL);
}
