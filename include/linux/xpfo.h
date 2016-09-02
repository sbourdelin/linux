/*
 * Copyright (C) 2016 Hewlett Packard Enterprise Development, L.P.
 * Copyright (C) 2016 Brown University. All rights reserved.
 *
 * Authors:
 *   Juerg Haefliger <juerg.haefliger@hpe.com>
 *   Vasileios P. Kemerlis <vpk@cs.brown.edu>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#ifndef _LINUX_XPFO_H
#define _LINUX_XPFO_H

#ifdef CONFIG_XPFO

extern struct page_ext_operations page_xpfo_ops;

extern void xpfo_kmap(void *kaddr, struct page *page);
extern void xpfo_kunmap(void *kaddr, struct page *page);
extern void xpfo_alloc_page(struct page *page, int order, gfp_t gfp);
extern void xpfo_free_page(struct page *page, int order);

extern bool xpfo_page_is_unmapped(struct page *page);
extern bool xpfo_page_is_kernel(struct page *page);

#else /* !CONFIG_XPFO */

static inline void xpfo_kmap(void *kaddr, struct page *page) { }
static inline void xpfo_kunmap(void *kaddr, struct page *page) { }
static inline void xpfo_alloc_page(struct page *page, int order, gfp_t gfp) { }
static inline void xpfo_free_page(struct page *page, int order) { }

static inline bool xpfo_page_is_unmapped(struct page *page) { return false; }
static inline bool xpfo_page_is_kernel(struct page *page) { return false; }

#endif /* CONFIG_XPFO */

#endif /* _LINUX_XPFO_H */
