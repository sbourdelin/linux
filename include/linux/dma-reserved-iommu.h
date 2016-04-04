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

#ifdef __KERNEL__
#include <asm/errno.h>

#ifdef CONFIG_IOMMU_DMA_RESERVED
#include <linux/iommu.h>

/**
 * iommu_alloc_reserved_iova_domain: allocate the reserved iova domain
 *
 * @domain: iommu domain handle
 * @iova: base iova address
 * @size: iova window size
 * @order: page order
 */
int iommu_alloc_reserved_iova_domain(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size,
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
 * where order corresponds to the iova domain order.
 * This mapping is reference counted as a whole and cannot by split.
 */
int iommu_get_reserved_iova(struct iommu_domain *domain,
			      phys_addr_t addr, size_t size, int prot,
			      dma_addr_t *iova);

/**
 * iommu_put_reserved_iova: decrement a ref count of the reserved mapping
 *
 * @domain: iommu domain handle
 * @iova: reserved iova whose binding ref count is decremented
 *
 * if the binding ref count is null, destroy the reserved mapping
 */
void iommu_put_reserved_iova(struct iommu_domain *domain, dma_addr_t iova);

/**
 * iommu_unmap_reserved: unmap & destroy the reserved iova bindings
 *
 * @domain: iommu domain handle
 */
void iommu_unmap_reserved(struct iommu_domain *domain);

#endif	/* CONFIG_IOMMU_DMA_RESERVED */
#endif	/* __KERNEL__ */
#endif	/* __DMA_RESERVED_IOMMU_H */
