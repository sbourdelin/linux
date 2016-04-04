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

#endif	/* CONFIG_IOMMU_DMA_RESERVED */
#endif	/* __KERNEL__ */
#endif	/* __DMA_RESERVED_IOMMU_H */
