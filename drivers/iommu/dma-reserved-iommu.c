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

/* called with domain's reserved_lock held */
static void reserved_binding_release(struct kref *kref)
{
	struct iommu_reserved_binding *b =
		container_of(kref, struct iommu_reserved_binding, kref);
	struct iommu_domain *d = b->domain;
	struct reserved_iova_domain *rid =
		(struct reserved_iova_domain *)d->reserved_iova_cookie;
	unsigned long order;

	order = iova_shift(rid->iovad);
	free_iova(rid->iovad, b->iova >> order);
	unlink_reserved_binding(d, b);
	kfree(b);
}

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

int iommu_get_reserved_iova(struct iommu_domain *domain,
			      phys_addr_t addr, size_t size, int prot,
			      dma_addr_t *iova)
{
	unsigned long base_pfn, end_pfn, nb_iommu_pages, order, flags;
	struct iommu_reserved_binding *b, *newb;
	size_t iommu_page_size, binding_size;
	phys_addr_t aligned_base, offset;
	struct reserved_iova_domain *rid;
	struct iova_domain *iovad;
	struct iova *p_iova;
	int ret = -EINVAL;

	newb = kzalloc(sizeof(*newb), GFP_KERNEL);
	if (!newb)
		return -ENOMEM;

	spin_lock_irqsave(&domain->reserved_lock, flags);

	rid = (struct reserved_iova_domain *)domain->reserved_iova_cookie;
	if (!rid)
		goto free_newb;

	if ((prot & IOMMU_READ & !(rid->prot & IOMMU_READ)) ||
	    (prot & IOMMU_WRITE & !(rid->prot & IOMMU_WRITE)))
		goto free_newb;

	iovad = rid->iovad;
	order = iova_shift(iovad);
	base_pfn = addr >> order;
	end_pfn = (addr + size - 1) >> order;
	aligned_base = base_pfn << order;
	offset = addr - aligned_base;
	nb_iommu_pages = end_pfn - base_pfn + 1;
	iommu_page_size = 1 << order;
	binding_size = nb_iommu_pages * iommu_page_size;

	b = find_reserved_binding(domain, aligned_base, binding_size);
	if (b) {
		*iova = b->iova + offset + aligned_base - b->addr;
		kref_get(&b->kref);
		ret = 0;
		goto free_newb;
	}

	p_iova = alloc_iova(iovad, nb_iommu_pages,
			    iovad->dma_32bit_pfn, true);
	if (!p_iova) {
		ret = -ENOMEM;
		goto free_newb;
	}

	*iova = iova_dma_addr(iovad, p_iova);

	/* unlock to call iommu_map which is not guaranteed to be atomic */
	spin_unlock_irqrestore(&domain->reserved_lock, flags);

	ret = iommu_map(domain, *iova, aligned_base, binding_size, prot);

	spin_lock_irqsave(&domain->reserved_lock, flags);

	rid = (struct reserved_iova_domain *) domain->reserved_iova_cookie;
	if (!rid || (rid->iovad != iovad)) {
		/* reserved iova domain was destroyed in our back */
		ret = -EBUSY;
		goto free_newb; /* iova already released */
	}

	/* no change in iova reserved domain but iommu_map failed */
	if (ret)
		goto free_iova;

	/* everything is fine, add in the new node in the rb tree */
	kref_init(&newb->kref);
	newb->domain = domain;
	newb->addr = aligned_base;
	newb->iova = *iova;
	newb->size = binding_size;

	link_reserved_binding(domain, newb);

	*iova += offset;
	goto unlock;

free_iova:
	free_iova(rid->iovad, p_iova->pfn_lo);
free_newb:
	kfree(newb);
unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(iommu_get_reserved_iova);

void iommu_put_reserved_iova(struct iommu_domain *domain, phys_addr_t addr)
{
	phys_addr_t aligned_addr, page_size, mask;
	struct iommu_reserved_binding *b;
	struct reserved_iova_domain *rid;
	unsigned long order, flags;
	struct iommu_domain *d;
	dma_addr_t iova;
	size_t size;
	int ret = 0;

	spin_lock_irqsave(&domain->reserved_lock, flags);

	rid = (struct reserved_iova_domain *)domain->reserved_iova_cookie;
	if (!rid)
		goto unlock;

	order = iova_shift(rid->iovad);
	page_size = (uint64_t)1 << order;
	mask = page_size - 1;
	aligned_addr = addr & ~mask;

	b = find_reserved_binding(domain, aligned_addr, page_size);
	if (!b)
		goto unlock;

	iova = b->iova;
	size = b->size;
	d = b->domain;

	ret = kref_put(&b->kref, reserved_binding_release);

unlock:
	spin_unlock_irqrestore(&domain->reserved_lock, flags);
	if (ret)
		iommu_unmap(d, iova, size);
}
EXPORT_SYMBOL_GPL(iommu_put_reserved_iova);

