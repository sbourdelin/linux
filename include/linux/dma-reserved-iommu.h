/*
 * Copyright (c) 2015 Linaro Ltd.
 *              www.linaro.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#ifndef __DMA_RESERVED_IOMMU_H
#define __DMA_RESERVED_IOMMU_H

#include <linux/types.h>
#include <linux/kernel.h>

struct iommu_domain;

#ifdef CONFIG_IOMMU_DMA_RESERVED

/**
 * iommu_alloc_reserved_iova_domain: allocate the reserved iova domain
 *
 * @domain: iommu domain handle
 * @iova: base iova address
 * @size: iova window size
 * @prot: protection attribute flags
 * @order: page order
 */
int iommu_alloc_reserved_iova_domain(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size, int prot,
				     unsigned long order);

/**
 * iommu_free_reserved_iova_domain: free the reserved iova domain
 *
 * @domain: iommu domain handle
 */
void iommu_free_reserved_iova_domain(struct iommu_domain *domain);

#else

static inline int
iommu_alloc_reserved_iova_domain(struct iommu_domain *domain,
				 dma_addr_t iova, size_t size, int prot,
				 unsigned long order)
{
	return -ENOENT;
}

static inline void
iommu_free_reserved_iova_domain(struct iommu_domain *domain) {}

#endif	/* CONFIG_IOMMU_DMA_RESERVED */
#endif	/* __DMA_RESERVED_IOMMU_H */
