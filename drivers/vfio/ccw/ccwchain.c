/*
 * ccwchain interfaces
 *
 * Copyright IBM Corp. 2016
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License (version 2 only)
 * as published by the Free Software Foundation.
 *
 * Author(s): Dong Jia Shi <bjsdjshi@linux.vnet.ibm.com>
 *            Xiao Feng Ren <renxiaof@linux.vnet.ibm.com>
 */

#include <linux/mm.h>
#include <linux/slab.h>

struct page_array {
	u64			hva;
	int			nr;
	struct page		**items;
};

struct page_arrays {
	struct page_array	*parray;
	int			nr;
};

/*
 * Helpers to operate page_array.
 */
/*
 * page_array_pin() - pin user pages in memory
 * @p: page_array on which to perform the operation
 *
 * Attempt to pin user pages in memory.
 *
 * Usage of page_array:
 * @p->hva      starting user address. Assigned by caller.
 * @p->nr       number of pages from @p->hva to pin. Assigned by caller.
 *              number of pages pinned. Assigned by callee.
 * @p->items    array that receives pointers to the pages pinned. Allocated by
 *              caller.
 *
 * Returns:
 *   Number of pages pinned on success. If @p->nr is 0 or negative, returns 0.
 *   If no pages were pinned, returns -errno.
 */
static int page_array_pin(struct page_array *p)
{
	int i, nr;

	nr = get_user_pages_fast(p->hva, p->nr, 1, p->items);
	if (nr <= 0) {
		p->nr = 0;
		return nr;
	} else if (nr != p->nr) {
		for (i = 0; i < nr; i++)
			put_page(p->items[i]);
		p->nr = 0;
		return -ENOMEM;
	}

	return nr;
}

/* Unpin the items before releasing the memory. */
static void page_array_items_unpin_free(struct page_array *p)
{
	int i;

	for (i = 0; i < p->nr; i++)
		put_page(p->items[i]);

	p->nr = 0;
	kfree(p->items);
}

/* Alloc memory for items, then pin pages with them. */
static int page_array_items_alloc_pin(u64 hva,
				      unsigned int len,
				      struct page_array *p)
{
	int ret;

	if (!len || p->nr)
		return -EINVAL;

	p->hva = hva;

	p->nr = ((hva & ~PAGE_MASK) + len + (PAGE_SIZE - 1)) >> PAGE_SHIFT;
	if (!p->nr)
		return -EINVAL;

	p->items = kcalloc(p->nr, sizeof(*p->items), GFP_KERNEL);
	if (!p->items)
		return -ENOMEM;

	ret = page_array_pin(p);
	if (ret <= 0)
		kfree(p->items);

	return ret;
}

static int page_arrays_init(struct page_arrays *ps, int nr)
{
	ps->parray = kcalloc(nr, sizeof(*ps->parray), GFP_KERNEL);
	if (!ps->parray) {
		ps->nr = 0;
		return -ENOMEM;
	}

	ps->nr = nr;
	return 0;
}

static void page_arrays_unpin_free(struct page_arrays *ps)
{
	int i;

	for (i = 0; i < ps->nr; i++)
		page_array_items_unpin_free(ps->parray + i);

	kfree(ps->parray);

	ps->parray = NULL;
	ps->nr = 0;
}
