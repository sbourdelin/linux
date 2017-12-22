/*
 * eXtensible Bitmaps
 * Copyright (c) 2017 Microsoft Corporation <mawilcox@microsoft.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * eXtensible Bitmaps provide an unlimited-size sparse bitmap facility.
 * All bits are initially zero.
 */

#include <linux/idr.h>

struct xb {
	struct radix_tree_root xbrt;
};

#define XB_INIT {							\
	.xbrt = RADIX_TREE_INIT(IDR_RT_MARKER | GFP_NOWAIT),		\
}
#define DEFINE_XB(name)		struct xb name = XB_INIT

static inline void xb_init(struct xb *xb)
{
	INIT_RADIX_TREE(&xb->xbrt, IDR_RT_MARKER | GFP_NOWAIT);
}

int xb_set_bit(struct xb *xb, unsigned long bit);
bool xb_test_bit(const struct xb *xb, unsigned long bit);
void xb_clear_bit(struct xb *xb, unsigned long bit);
void xb_clear_bit_range(struct xb *xb, unsigned long start,
			unsigned long nbits);
unsigned long xb_find_set(struct xb *xb, unsigned long size,
			  unsigned long offset);
unsigned long xb_find_zero(struct xb *xb, unsigned long size,
			   unsigned long offset);

static inline bool xb_empty(const struct xb *xb)
{
	return radix_tree_empty(&xb->xbrt);
}

int xb_preload(gfp_t);

static inline void xb_preload_end(void)
{
	preempt_enable();
}
