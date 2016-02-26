/*
 * Reserved IOVA Management
 *
 * Copyright (c) 2015 Linaro Ltd.
 *              www.linaro.org
 *
 * Copyright (C) 2000-2004 Russell King
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

struct iommu_reserved_binding {
	struct kref		kref;
	struct rb_node		node;
	struct iommu_domain	*domain;
	phys_addr_t		addr;
	dma_addr_t		iova;
	size_t			size;
};

/* Reserved binding RB-tree manipulation */

static struct iommu_reserved_binding *find_reserved_binding(
				    struct iommu_domain *d,
				    phys_addr_t start, size_t size)
{
	struct rb_node *node = d->reserved_binding_list.rb_node;

	while (node) {
		struct iommu_reserved_binding *binding =
			rb_entry(node, struct iommu_reserved_binding, node);

		if (start + size <= binding->addr)
			node = node->rb_left;
		else if (start >= binding->addr + binding->size)
			node = node->rb_right;
		else
			return binding;
	}

	return NULL;
}

static void link_reserved_binding(struct iommu_domain *d,
				  struct iommu_reserved_binding *new)
{
	struct rb_node **link = &d->reserved_binding_list.rb_node;
	struct rb_node *parent = NULL;
	struct iommu_reserved_binding *binding;

	while (*link) {
		parent = *link;
		binding = rb_entry(parent, struct iommu_reserved_binding,
				   node);

		if (new->addr + new->size <= binding->addr)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&new->node, parent, link);
	rb_insert_color(&new->node, &d->reserved_binding_list);
}

static void unlink_reserved_binding(struct iommu_domain *d,
				    struct iommu_reserved_binding *old)
{
	rb_erase(&old->node, &d->reserved_binding_list);
}

int iommu_alloc_reserved_iova_domain(struct iommu_domain *domain,
				     dma_addr_t iova, size_t size,
				     unsigned long order)
{
	unsigned long granule, mask;
	struct iova_domain *iovad;
	int ret = 0;

	granule = 1UL << order;
	mask = granule - 1;
	if (iova & mask || (!size) || (size & mask))
		return -EINVAL;

	mutex_lock(&domain->reserved_mutex);

	if (domain->reserved_iova_cookie) {
		ret = -EEXIST;
		goto unlock;
	}

	iovad = kzalloc(sizeof(struct iova_domain), GFP_KERNEL);
	if (!iovad) {
		ret = -ENOMEM;
		goto unlock;
	}

	init_iova_domain(iovad, granule,
			 iova >> order, (iova + size - 1) >> order);
	domain->reserved_iova_cookie = iovad;

unlock:
	mutex_unlock(&domain->reserved_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_alloc_reserved_iova_domain);

void iommu_free_reserved_iova_domain(struct iommu_domain *domain)
{
	struct iova_domain *iovad =
		(struct iova_domain *)domain->reserved_iova_cookie;

	if (!iovad)
		return;

	mutex_lock(&domain->reserved_mutex);

	put_iova_domain(iovad);
	kfree(iovad);

	mutex_unlock(&domain->reserved_mutex);
}
EXPORT_SYMBOL_GPL(iommu_free_reserved_iova_domain);

int iommu_get_single_reserved(struct iommu_domain *domain,
			      phys_addr_t addr, int prot,
			      dma_addr_t *iova)
{
	unsigned long order = __ffs(domain->ops->pgsize_bitmap);
	size_t page_size = 1 << order;
	phys_addr_t mask = page_size - 1;
	phys_addr_t aligned_addr = addr & ~mask;
	phys_addr_t offset  = addr - aligned_addr;
	struct iommu_reserved_binding *b;
	struct iova *p_iova;
	struct iova_domain *iovad =
		(struct iova_domain *)domain->reserved_iova_cookie;
	int ret;

	if (!iovad)
		return -EINVAL;

	mutex_lock(&domain->reserved_mutex);

	b = find_reserved_binding(domain, aligned_addr, page_size);
	if (b) {
		*iova = b->iova + offset;
		kref_get(&b->kref);
		ret = 0;
		goto unlock;
	}

	/* there is no existing reserved iova for this pa */
	p_iova = alloc_iova(iovad, 1, iovad->dma_32bit_pfn, true);
	if (!p_iova) {
		ret = -ENOMEM;
		goto unlock;
	}
	*iova = p_iova->pfn_lo << order;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b) {
		ret = -ENOMEM;
		goto free_iova_unlock;
	}

	ret = iommu_map(domain, *iova, aligned_addr, page_size, prot);
	if (ret)
		goto free_binding_iova_unlock;

	kref_init(&b->kref);
	kref_get(&b->kref);
	b->domain = domain;
	b->addr = aligned_addr;
	b->iova = *iova;
	b->size = page_size;

	link_reserved_binding(domain, b);

	*iova += offset;
	goto unlock;

free_binding_iova_unlock:
	kfree(b);
free_iova_unlock:
	free_iova(iovad, *iova >> order);
unlock:
	mutex_unlock(&domain->reserved_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_get_single_reserved);

/* called with reserved_mutex locked */
static void reserved_binding_release(struct kref *kref)
{
	struct iommu_reserved_binding *b =
		container_of(kref, struct iommu_reserved_binding, kref);
	struct iommu_domain *d = b->domain;
	struct iova_domain *iovad =
		(struct iova_domain *)d->reserved_iova_cookie;
	unsigned long order = __ffs(b->size);

	iommu_unmap(d, b->iova, b->size);
	free_iova(iovad, b->iova >> order);
	unlink_reserved_binding(d, b);
	kfree(b);
}

void iommu_put_single_reserved(struct iommu_domain *domain, dma_addr_t iova)
{
	unsigned long order;
	phys_addr_t aligned_addr;
	dma_addr_t aligned_iova, page_size, mask, offset;
	struct iommu_reserved_binding *b;

	order = __ffs(domain->ops->pgsize_bitmap);
	page_size = (uint64_t)1 << order;
	mask = page_size - 1;

	aligned_iova = iova & ~mask;
	offset = iova - aligned_iova;

	aligned_addr = iommu_iova_to_phys(domain, aligned_iova);

	mutex_lock(&domain->reserved_mutex);

	b = find_reserved_binding(domain, aligned_addr, page_size);
	if (!b)
		goto unlock;
	kref_put(&b->kref, reserved_binding_release);

unlock:
	mutex_unlock(&domain->reserved_mutex);
}
EXPORT_SYMBOL_GPL(iommu_put_single_reserved);



