/*
 * pmalloc-selftest.c
 *
 * (C) Copyright 2018 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#include <linux/pmalloc.h>
#include <linux/mm.h>


#define SIZE_1 (PAGE_SIZE * 3)
#define SIZE_2 1000

#define validate_alloc(expected, variable, size)	\
	pr_notice("must be " expected ": %s",		\
		  is_pmalloc_object(variable, size) > 0 ? "ok" : "no")

#define is_alloc_ok(variable, size)	\
	validate_alloc("ok", variable, size)

#define is_alloc_no(variable, size)	\
	validate_alloc("no", variable, size)

void pmalloc_selftest(void)
{
	struct gen_pool *pool_unprot;
	struct gen_pool *pool_prot;
	void *var_prot, *var_unprot, *var_vmall;

	pr_notice("pmalloc self-test");
	pool_unprot = pmalloc_create_pool("unprotected", 0);
	pool_prot = pmalloc_create_pool("protected", 0);
	BUG_ON(!(pool_unprot && pool_prot));

	var_unprot = pmalloc(pool_unprot,  SIZE_1 - 1, GFP_KERNEL);
	var_prot = pmalloc(pool_prot,  SIZE_1, GFP_KERNEL);
	var_vmall = vmalloc(SIZE_2);
	is_alloc_ok(var_unprot, 10);
	is_alloc_ok(var_unprot, SIZE_1);
	is_alloc_ok(var_unprot, PAGE_SIZE);
	is_alloc_no(var_unprot, SIZE_1 + 1);
	is_alloc_no(var_vmall, 10);


	pfree(pool_unprot, var_unprot);
	vfree(var_vmall);

	pmalloc_protect_pool(pool_prot);

	/* This will intentionally trigger a WARN because the pool being
	 * destroyed is not protected, which is unusual and should happen
	 * on error paths only, where probably other warnings are already
	 * displayed.
	 */
	pmalloc_destroy_pool(pool_unprot);

	/* This must not cause WARNings */
	pmalloc_destroy_pool(pool_prot);
}
