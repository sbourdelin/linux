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
struct msi_desc;
struct irq_data;
struct msi_msg;

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

/**
 * iommu_msi_mapping_desc_to_domain: in case the MSI originates from a device
 * upstream to an IOMMU and this IOMMU translates the MSI transaction,
 * this function returns the iommu domain the MSI doorbell address must be
 * mapped in. Else it returns NULL.
 *
 * @desc: msi desc handle
 */
struct iommu_domain *iommu_msi_mapping_desc_to_domain(struct msi_desc *desc);

/**
 * iommu_msi_mapping_translate_msg: in case the MSI transaction is translated
 * by an IOMMU, the msg address must be an IOVA instead of a physical address.
 * This function overwrites the original MSI message containing the doorbell
 * physical address, result of the primary composition, with the doorbell IOVA.
 *
 * The doorbell physical address must be bound previously to an IOVA using
 * iommu_get_reserved_iova
 *
 * @data: irq data handle
 * @msg: original msi message containing the PA to be overwritten with
 * the IOVA
 *
 * return 0 if the MSI does not need to be mapped or when the PA/IOVA
 * were successfully swapped; return -EINVAL if the addresses need
 * to be swapped but not IOMMU binding is found
 */
int iommu_msi_mapping_translate_msg(struct irq_data *data, struct msi_msg *msg);

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

static inline struct iommu_domain *
iommu_msi_mapping_desc_to_domain(struct msi_desc *desc)
{
	return NULL;
}

static inline int iommu_msi_mapping_translate_msg(struct irq_data *data,
						  struct msi_msg *msg)
{
	return 0;
}

#endif	/* CONFIG_IOMMU_DMA_RESERVED */
#endif	/* __DMA_RESERVED_IOMMU_H */
