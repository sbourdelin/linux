/*
 * Reserved IOVA Management
 *
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

#include <linux/iommu.h>
#include <linux/iova.h>

int iommu_alloc_reserved_iova_domain(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size,
				     unsigned long order)
{
	unsigned long granule, mask;
	struct iova_domain *iovad;
	unsigned long flags;
	int ret = 0;

	granule = 1UL << order;
	mask = granule - 1;
	if (iova & mask || (!size) || (size & mask))
		return -EINVAL;

	iovad = kzalloc(sizeof(struct iova_domain), GFP_KERNEL);
	if (!iovad)
		return -ENOMEM;

	spin_lock_irqsave(&domain->reserved_lock, flags);

	if (domain->reserved_iova_cookie) {
		ret = -EEXIST;
		goto free_unlock;
	}

	init_iova_domain(iovad, granule,
			 iova >> order, (iova + size - 1) >> order);
	domain->reserved_iova_cookie = iovad;
	goto unlock;

free_unlock:
	kfree(iovad);
unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_alloc_reserved_iova_domain);

void iommu_free_reserved_iova_domain(struct iommu_domain *domain)
{
	struct iova_domain *iovad =
		(struct iova_domain *)domain->reserved_iova_cookie;
	unsigned long flags;

	if (!iovad)
		return;

	spin_lock_irqsave(&domain->reserved_lock, flags);

	put_iova_domain(iovad);
	kfree(iovad);

	spin_unlock_irqrestore(&domain->reserved_lock, flags);
}
EXPORT_SYMBOL_GPL(iommu_free_reserved_iova_domain);
