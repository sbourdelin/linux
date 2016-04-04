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

struct iommu_reserved_binding {
	struct kref		kref;
	struct rb_node		node;
	struct iommu_domain	*domain;
	phys_addr_t		addr;
	dma_addr_t		iova;
	size_t			size;
};

/* Reserved binding RB-tree manipulation */

/* @d->reserved_lock must be held */
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

/* @d->reserved_lock must be held */
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

/* @d->reserved_lock must be held */
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

static void delete_reserved_binding(struct iommu_domain *domain,
				    struct iommu_reserved_binding *b)
{
	struct iova_domain *iovad =
		(struct iova_domain *)domain->reserved_iova_cookie;
	unsigned long order = iova_shift(iovad);

	iommu_unmap(domain, b->iova, b->size);
	free_iova(iovad, b->iova >> order);
	kfree(b);
}

int iommu_get_reserved_iova(struct iommu_domain *domain,
			      phys_addr_t addr, size_t size, int prot,
			      dma_addr_t *iova)
{
	struct iova_domain *iovad =
		(struct iova_domain *)domain->reserved_iova_cookie;
	unsigned long order = iova_shift(iovad);
	unsigned long  base_pfn, end_pfn, nb_iommu_pages;
	size_t iommu_page_size = 1 << order, binding_size;
	phys_addr_t aligned_base, offset;
	struct iommu_reserved_binding *b, *newb;
	unsigned long flags;
	struct iova *p_iova;
	bool unmap = false;
	int ret;

	base_pfn = addr >> order;
	end_pfn = (addr + size - 1) >> order;
	nb_iommu_pages = end_pfn - base_pfn + 1;
	aligned_base = base_pfn << order;
	offset = addr - aligned_base;
	binding_size = nb_iommu_pages * iommu_page_size;

	if (!iovad)
		return -EINVAL;

	spin_lock_irqsave(&domain->reserved_lock, flags);

	b = find_reserved_binding(domain, aligned_base, binding_size);
	if (b) {
		*iova = b->iova + offset;
		kref_get(&b->kref);
		ret = 0;
		goto unlock;
	}

	spin_unlock_irqrestore(&domain->reserved_lock, flags);

	/*
	 * no reserved IOVA was found for this PA, start allocating and
	 * registering one while the spin-lock is not held. iommu_map/unmap
	 * are not supposed to be atomic
	 */

	p_iova = alloc_iova(iovad, nb_iommu_pages, iovad->dma_32bit_pfn, true);
	if (!p_iova)
		return -ENOMEM;

	*iova = iova_dma_addr(iovad, p_iova);

	newb = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!newb) {
		free_iova(iovad, p_iova->pfn_lo);
		return -ENOMEM;
	}

	ret = iommu_map(domain, *iova, aligned_base, binding_size, prot);
	if (ret) {
		kfree(newb);
		free_iova(iovad, p_iova->pfn_lo);
		return ret;
	}

	spin_lock_irqsave(&domain->reserved_lock, flags);

	/* re-check the PA was not mapped in our back when lock was not held */
	b = find_reserved_binding(domain, aligned_base, binding_size);
	if (b) {
		*iova = b->iova + offset;
		kref_get(&b->kref);
		ret = 0;
		unmap = true;
		goto unlock;
	}

	kref_init(&newb->kref);
	newb->domain = domain;
	newb->addr = aligned_base;
	newb->iova = *iova;
	newb->size = binding_size;

	link_reserved_binding(domain, newb);

	*iova += offset;
	goto unlock;

unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	if (unmap)
		delete_reserved_binding(domain, newb);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_get_reserved_iova);

void iommu_put_reserved_iova(struct iommu_domain *domain, dma_addr_t iova)
{
	struct iova_domain *iovad =
		(struct iova_domain *)domain->reserved_iova_cookie;
	unsigned long order;
	phys_addr_t aligned_addr;
	dma_addr_t aligned_iova, page_size, mask, offset;
	struct iommu_reserved_binding *b;
	unsigned long flags;
	bool unmap = false;

	order = iova_shift(iovad);
	page_size = (uint64_t)1 << order;
	mask = page_size - 1;

	aligned_iova = iova & ~mask;
	offset = iova - aligned_iova;

	aligned_addr = iommu_iova_to_phys(domain, aligned_iova);

	spin_lock_irqsave(&domain->reserved_lock, flags);
	b = find_reserved_binding(domain, aligned_addr, page_size);
	if (!b)
		goto unlock;

	if (atomic_sub_and_test(1, &b->kref.refcount)) {
		unlink_reserved_binding(domain, b);
		unmap = true;
	}

unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	if (unmap)
		delete_reserved_binding(domain, b);
}
EXPORT_SYMBOL_GPL(iommu_put_reserved_iova);



