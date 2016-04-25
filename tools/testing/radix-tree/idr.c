/*
 * idr.c: Test the IDR API
 * Copyright (c) 2016 Intel Corporation
 * Author: Matthew Wilcox <willy@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/errno.h>

#include "test.h"

int item_idr_free(int id, void *p, void *data)
{
	struct item *item = p;
	assert(item->index == id);
	free(p);

	return 0;
}

void idr_simple_check(void)
{
	DEFINE_IDR(idr);

	unsigned long i;

	for (i = 0; i < 10000; i++) {
		struct item *item = item_create(i);
		assert(idr_alloc(&idr, item, 0, 20000, GFP_KERNEL) == i);
	}

	assert(idr_alloc(&idr, idr_simple_check, 5, 30, GFP_KERNEL) < 0);

	for (i = 0; i < 5000; i++)
		idr_remove(&idr, i);

	idr_for_each(&idr, item_idr_free, NULL);

	idr_destroy(&idr);

	assert(idr_is_empty(&idr));
}

void idr_checks(void)
{
	idr_simple_check();
}
