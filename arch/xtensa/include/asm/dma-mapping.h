/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2003 - 2005 Tensilica Inc.
 * Copyright (C) 2015 Cadence Design Systems Inc.
 */

#ifndef _XTENSA_DMA_MAPPING_H
#define _XTENSA_DMA_MAPPING_H

#include <asm/cache.h>
#include <asm/io.h>

#include <linux/mm.h>
#include <linux/scatterlist.h>

#define DMA_ERROR_CODE		(~(dma_addr_t)0x0)

extern struct dma_map_ops xtensa_dma_map_ops;

static inline struct dma_map_ops *get_dma_ops(struct device *dev)
{
	if (dev && dev->archdata.dma_ops)
		return dev->archdata.dma_ops;
	else
		return &xtensa_dma_map_ops;
}

void dma_cache_sync(struct device *dev, void *vaddr, size_t size,
		    enum dma_data_direction direction);

static inline dma_addr_t swiotlb_phys_to_dma(struct device *dev,
					     phys_addr_t paddr)
{
	return (dma_addr_t)paddr;
}
#define swiotlb_phys_to_dma swiotlb_phys_to_dma

static inline phys_addr_t swiotlb_dma_to_phys(struct device *dev,
					      dma_addr_t daddr)
{
	return (phys_addr_t)daddr;
}
#define swiotlb_dma_to_phys swiotlb_dma_to_phys

#endif	/* _XTENSA_DMA_MAPPING_H */
