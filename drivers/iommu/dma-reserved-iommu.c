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

struct reserved_iova_domain {
	struct iova_domain *iovad;
	int prot; /* iommu protection attributes to be obeyed */
};

int iommu_alloc_reserved_iova_domain(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size, int prot,
				     unsigned long order)
{
	unsigned long granule, mask, flags;
	struct reserved_iova_domain *rid;
	int ret = 0;

	granule = 1UL << order;
	mask = granule - 1;
	if (iova & mask || (!size) || (size & mask))
		return -EINVAL;

	rid = kzalloc(sizeof(struct reserved_iova_domain), GFP_KERNEL);
	if (!rid)
		return -ENOMEM;

	rid->iovad = kzalloc(sizeof(struct iova_domain), GFP_KERNEL);
	if (!rid->iovad) {
		kfree(rid);
		return -ENOMEM;
	}

	iova_cache_get();

	init_iova_domain(rid->iovad, granule,
			 iova >> order, (iova + size - 1) >> order);

	spin_lock_irqsave(&domain->reserved_lock, flags);

	if (domain->reserved_iova_cookie) {
		ret = -EEXIST;
		goto unlock;
	}

	domain->reserved_iova_cookie = rid;

unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	if (ret) {
		put_iova_domain(rid->iovad);
		kfree(rid->iovad);
		kfree(rid);
		iova_cache_put();
	}
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_alloc_reserved_iova_domain);

void iommu_free_reserved_iova_domain(struct iommu_domain *domain)
{
	struct reserved_iova_domain *rid;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&domain->reserved_lock, flags);

	rid = (struct reserved_iova_domain *)domain->reserved_iova_cookie;
	if (!rid) {
		ret = -EINVAL;
		goto unlock;
	}

	domain->reserved_iova_cookie = NULL;
unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	if (!ret) {
		put_iova_domain(rid->iovad);
		kfree(rid->iovad);
		kfree(rid);
		iova_cache_put();
	}
}
EXPORT_SYMBOL_GPL(iommu_free_reserved_iova_domain);
