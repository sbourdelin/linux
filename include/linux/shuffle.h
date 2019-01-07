// SPDX-License-Identifier: GPL-2.0
// Copyright(c) 2018 Intel Corporation. All rights reserved.
#ifndef _MM_SHUFFLE_H
#define _MM_SHUFFLE_H
#include <linux/jump_label.h>

enum mm_shuffle_ctl {
	SHUFFLE_ENABLE,
	SHUFFLE_FORCE_DISABLE,
};
#ifdef CONFIG_SHUFFLE_PAGE_ALLOCATOR
DECLARE_STATIC_KEY_FALSE(page_alloc_shuffle_key);
extern void page_alloc_shuffle(enum mm_shuffle_ctl ctl);
extern void __shuffle_free_memory(pg_data_t *pgdat, unsigned long start_pfn,
		unsigned long end_pfn);
static inline void shuffle_free_memory(pg_data_t *pgdat,
		unsigned long start_pfn, unsigned long end_pfn)
{
	if (!static_branch_unlikely(&page_alloc_shuffle_key))
		return;
	__shuffle_free_memory(pgdat, start_pfn, end_pfn);
}

extern void __shuffle_zone(struct zone *z, unsigned long start_pfn,
		unsigned long end_pfn);
static inline void shuffle_zone(struct zone *z, unsigned long start_pfn,
		unsigned long end_pfn)
{
	if (!static_branch_unlikely(&page_alloc_shuffle_key))
		return;
	__shuffle_zone(z, start_pfn, end_pfn);
}

static inline bool is_shuffle_order(int order)
{
	if (!static_branch_unlikely(&page_alloc_shuffle_key))
                return false;
	return order >= CONFIG_SHUFFLE_PAGE_ORDER;
}
#else
static inline void shuffle_free_memory(pg_data_t *pgdat, unsigned long start_pfn,
		unsigned long end_pfn)
{
}

static inline void shuffle_zone(struct zone *z, unsigned long start_pfn,
		unsigned long end_pfn)
{
}

static inline void page_alloc_shuffle(enum mm_shuffle_ctl ctl)
{
}

static inline bool is_shuffle_order(int order)
{
	return false;
}
#endif
#endif /* _MM_SHUFFLE_H */
