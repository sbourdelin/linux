// SPDX-License-Identifier: GPL-2.0
/*
 * sparsemask.c - sparse bitmap operations
 *
 * Copyright (c) 2018 Oracle Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/sparsemask.h>

/*
 * Return the next one bit in @mask after @start, not including @start.
 */
int sparsemask_next(int start, const struct sparsemask *mask)
{
	const unsigned long *addr = mask->bits;
	unsigned long nelems = mask->nelems;
	unsigned long tmp, nbits;

	/* -1 is a legal arg here. */
	if (start != -1)
		sparsemask_check(start, mask);
	start++;

	if (unlikely(start >= nelems))
		return nelems;

	start = SMASK_ELEM_TO_BIT(start, mask);
	nbits = SMASK_ELEM_TO_BIT(nelems, mask);
	tmp = addr[start / BITS_PER_LONG];

	/* Handle 1st word. */
	tmp &= BITMAP_FIRST_WORD_MASK(start);
	start = round_down(start, BITS_PER_LONG);

	while (!tmp) {
		start += SMASK_BITS;
		if (start >= nbits)
			return nelems;
		tmp = addr[start / BITS_PER_LONG];
	}

	return min(SMASK_BIT_TO_ELEM(start, mask) + __ffs(tmp), nelems);
}

int
sparsemask_next_wrap(int n, const struct sparsemask *mask, int start, bool wrap)
{
	int next;

again:
	next = sparsemask_next(n, mask);

	if (wrap && n < start && next >= start) {
		return mask->nelems;

	} else if (next >= mask->nelems) {
		wrap = true;
		n = -1;
		goto again;
	}

	return next;
}

unsigned int sparsemask_weight(const struct sparsemask *mask)
{
	int weight = 0;
	const unsigned long *addr = mask->bits;
	int nlongs = SMASK_ELEM_TO_LONG(mask->nelems, mask);
	int i, extra, shift;

	for (i = 0; i < nlongs; i += SMASK_LONGS) {
		if (addr[i])
			weight += hweight_long(addr[i]);
	}
	extra = mask->nelems - SMASK_LONG_TO_ELEM(i, mask);
	if (extra > 0) {
		shift = BITS_PER_LONG - extra;
		weight += hweight_long((addr[i] << shift) >> shift);
	}
	return weight;
}

size_t sparsemask_size(int nelems, int density)
{
	nelems = round_up(nelems, SMASK_DENSITY_TO_ELEMS(density));
	return sizeof(struct sparsemask) + _SMASK_ELEM_TO_BYTE(nelems, density);
}

void sparsemask_init(struct sparsemask *mask, int nelems, int density)
{
	WARN_ON(density < 0 || density > SMASK_DENSITY_MAX);
	mask->nelems = nelems;
	mask->density = density;
}

bool alloc_sparsemask_node(struct sparsemask **mask, int nelems, int density,
			   gfp_t flags, int node)
{
	*mask = kmalloc_node(sparsemask_size(nelems, density), flags, node);
	if (*mask)
		sparsemask_init(*mask, nelems, density);
	return !!*mask;
}

bool zalloc_sparsemask_node(struct sparsemask **mask, int nelems, int density,
			    gfp_t flags, int node)
{
	flags |= __GFP_ZERO;
	return alloc_sparsemask_node(mask, nelems, density, flags, node);
}

bool alloc_sparsemask(struct sparsemask **mask, int nelems, int density,
		      gfp_t flags)
{
	return alloc_sparsemask_node(mask, nelems, density, flags,
				     NUMA_NO_NODE);
}

bool zalloc_sparsemask(struct sparsemask **mask, int nelems, int density,
		       gfp_t flags)
{
	flags |= __GFP_ZERO;
	return alloc_sparsemask(mask, nelems, density, flags);
}

void free_sparsemask(struct sparsemask *mask)
{
	kfree(mask);
}
