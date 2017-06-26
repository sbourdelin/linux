/*
 * pmalloc.h: Header for Protectable Memory Allocator
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#ifndef _PMALLOC_H
#define _PMALLOC_H
#include <linux/genalloc.h>

#define PMALLOC_DEFAULT_ALLOC_ORDER (-1)

/**
 * pmalloc_create_pool - create a new protectable memory pool -
 * @name: the name of the pool, must be unique
 * @min_alloc_order: log2 of the minimum allocation size obtainable
 *                   from the pool
 *
 * Creates a new (empty) memory pool for allocation of protectable
 * memory. Memory will be allocated upon request (through pmalloc).
 *
 * Returns a pointer to the new pool, upon succes, otherwise a NULL.
 */
struct gen_pool *pmalloc_create_pool(const char *name,
					 int min_alloc_order);


/**
 * pmalloc_get_pool - get a pool handler, from its name
 * @name: the name of the pool sought after.
 *
 * Returns a pointer to the pool, upon succes, otherwise a NULL.
 */
struct gen_pool *pmalloc_get_pool(const char *name);



/**
 * pmalloc - allocate protectable memory from a pool
 * @pool: handler to the pool to be used for memory allocation
 * @size: amount of memory (in bytes) requested
 *
 * Allocates memory from an unprotected pool. If the pool doesn't have
 * enough memory, an attempt is made to add to the pool a new chunk of
 * memory (multiple of PAGE_SIZE) that can fit the new request.
 *
 * Returns the pointer to the memory requested, upon success,
 * NULL otherwise (either no memory availabel or pool RO).
 */
void *pmalloc(struct gen_pool *pool, size_t size);



/**
 * pmalloc_free - release memory previously obtained through pmalloc
 * @pool: the pool providing the memory
 * @addr: the memory address obtained from pmalloc
 * @size: the same amount of memory that was requested from pmalloc
 *
 * Releases the memory that was previously accounted for as in use.
 * It works also on pocked pools, but the memory released is simply
 * removed from the refcount of memory in use. It cannot be re-used.
 */
static __always_inline
void pmalloc_free(struct gen_pool *pool, void *addr, size_t size)
{
	gen_pool_free(pool, (unsigned long)addr, size);
}



/**
 * pmalloc_protect_pool - turn a RW pool into RO
 * @pool: the pool to protect
 *
 * Write protects all the memory chunks assigned to the pool.
 * This prevents further allocation.
 *
 * Returns 0 upon success, -EINVAL in abnormal cases.
 */
int pmalloc_protect_pool(struct gen_pool *pool);



/**
 * pmalloc_pool_protected - check if the pool is protected
 * @pool: the pool to test
 *
 * Returns true if the pool is either protected or missing. False otherwise.
 */
bool pmalloc_pool_protected(struct gen_pool *pool);



/**
 * pmalloc_destroy_pool - destroys a pool and all the associated memory
 * @pool: the pool to destroy
 *
 * All the memory that was allocated through pmalloc must first be freed
 * with pmalloc_free. Falire to do so will BUG().
 *
 * Returns 0 upon success, -EINVAL in abnormal cases.
 */
int pmalloc_destroy_pool(struct gen_pool *pool);
#endif
