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

/**
 * iommu_get_reserved_iova: allocate a contiguous set of iova pages and
 * map them to the physical range defined by @addr and @size.
 *
 * @domain: iommu domain handle
 * @addr: physical address to bind
 * @size: size of the binding
 * @prot: mapping protection attribute
 * @iova: returned iova
 *
 * Mapped physical pfns are within [@addr >> order, (@addr + size -1) >> order]
 * where order corresponds to the reserved iova domain order.
 * This mapping is tracked and reference counted with the minimal granularity
 * of @size.
 */
int iommu_get_reserved_iova(struct iommu_domain *domain,
			    phys_addr_t addr, size_t size, int prot,
			    dma_addr_t *iova);

/**
 * iommu_put_reserved_iova: decrement a ref count of the reserved mapping
 *
 * @domain: iommu domain handle
 * @addr: physical address whose binding ref count is decremented
 *
 * if the binding ref count is null, destroy the reserved mapping
 */
void iommu_put_reserved_iova(struct iommu_domain *domain, phys_addr_t addr);
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

static inline int iommu_get_reserved_iova(struct iommu_domain *domain,
					  phys_addr_t addr, size_t size,
					  int prot, dma_addr_t *iova)
{
	return -ENOENT;
}

static inline void iommu_put_reserved_iova(struct iommu_domain *domain,
					   phys_addr_t addr) {}

#endif	/* CONFIG_IOMMU_DMA_RESERVED */
#endif	/* __DMA_RESERVED_IOMMU_H */
