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
 * iommu_get_single_reserved: allocate a reserved iova page and bind
 * it onto the page that contains a physical address (@addr)
 *
 * @domain: iommu domain handle
 * @addr: physical address to bind
 * @prot: mapping protection attribute
 * @iova: returned iova
 *
 * In case the 2 pages already are bound simply return @iova and
 * increment a ref count
 */
int iommu_get_single_reserved(struct iommu_domain *domain,
			      phys_addr_t addr, int prot,
			      dma_addr_t *iova);

/**
 * iommu_put_single_reserved: decrement a ref count of the iova page
 *
 * @domain: iommu domain handle
 * @iova: iova whose binding ref count is decremented
 *
 * if the binding ref count is null, unmap the iova page and release the iova
 */
void iommu_put_single_reserved(struct iommu_domain *domain, dma_addr_t iova);

#endif	/* CONFIG_IOMMU_DMA_RESERVED */
#endif	/* __KERNEL__ */
#endif	/* __DMA_RESERVED_IOMMU_H */
