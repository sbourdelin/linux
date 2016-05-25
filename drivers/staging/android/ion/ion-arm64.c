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

#include <asm/cacheflush.h>

#include "ion_priv.h"

void ion_clean_page(struct page *page, size_t size)
{
	__flush_dcache_area(page_address(page), size);
}

void ion_invalidate_buffer(struct ion_buffer *buffer)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(buffer->sg_table->sgl, sg, buffer->sg_table->orig_nents, i)
		__dma_unmap_area(page_address(sg_page(sg)), sg->length,
					DMA_BIDIRECTIONAL);
}

void ion_clean_buffer(struct ion_buffer *buffer)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(buffer->sg_table->sgl, sg, buffer->sg_table->orig_nents, i)
		__dma_map_area(page_address(sg_page(sg)), sg->length,
					DMA_BIDIRECTIONAL);
}
