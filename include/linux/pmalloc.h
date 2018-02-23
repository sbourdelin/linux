/* SPDX-License-Identifier: GPL-2.0 */
/*
 * pmalloc.h: Header for Protectable Memory Allocator
 *
 * (C) Copyright 2017 Huawei Technologies Co. Ltd.
 * Author: Igor Stoppa <igor.stoppa@huawei.com>
 */

#ifndef _LINUX_PMALLOC_H
#define _LINUX_PMALLOC_H


#include <linux/genalloc.h>
#include <linux/string.h>

#define PMALLOC_DEFAULT_ALLOC_ORDER (-1)

/*
 * Library for dynamic allocation of pools of memory that can be,
 * after initialization, marked as read-only.
 *
 * This is intended to complement __read_only_after_init, for those cases
 * where either it is not possible to know the initialization value before
 * init is completed, or the amount of data is variable and can be
 * determined only at run-time.
 *
 * ***WARNING***
 * The user of the API is expected to synchronize:
 * 1) allocation,
 * 2) writes to the allocated memory,
 * 3) write protection of the pool,
 * 4) freeing of the allocated memory, and
 * 5) destruction of the pool.
 *
 * For a non-threaded scenario, this type of locking is not even required.
 *
 * Even if the library were to provide support for locking, point 2)
 * would still depend on the user taking the lock.
 */


/**
 * pmalloc_create_pool() - create a new protectable memory pool
 * @name: the name of the pool, enforced to be unique
 * @min_alloc_order: log2 of the minimum allocation size obtainable
 *                   from the pool
 *
 * Creates a new (empty) memory pool for allocation of protectable
 * memory. Memory will be allocated upon request (through pmalloc).
 *
 * Return:
 * * pointer to the new pool	- success
 * * NULL			- error
 */
struct gen_pool *pmalloc_create_pool(const char *name,
					 int min_alloc_order);

/**
 * is_pmalloc_object() - validates the existence of an alleged object
 * @ptr: address of the object
 * @n: size of the object, in bytes
 *
 * Return:
 * * 0		- the object does not belong to pmalloc
 * * 1		- the object belongs to pmalloc
 * * \-1	- the object overlaps pmalloc memory incorrectly
 */
int is_pmalloc_object(const void *ptr, const unsigned long n);

/**
 * pmalloc_prealloc() - tries to allocate a memory chunk of the requested size
 * @pool: handle to the pool to be used for memory allocation
 * @size: amount of memory (in bytes) requested
 *
 * Prepares a chunk of the requested size.
 * This is intended to both minimize latency in later memory requests and
 * avoid sleeping during allocation.
 * Memory allocated with prealloc is stored in one single chunk, as
 * opposed to what is allocated on-demand when pmalloc runs out of free
 * space already existing in the pool and has to invoke vmalloc.
 * One additional advantage of pre-allocating larger chunks of memory is
 * that the total slack tends to be smaller.
 *
 * Return:
 * * true	- the vmalloc call was successful
 * * false	- error
 */
bool pmalloc_prealloc(struct gen_pool *pool, size_t size);

/**
 * pmalloc() - allocate protectable memory from a pool
 * @pool: handle to the pool to be used for memory allocation
 * @size: amount of memory (in bytes) requested
 * @gfp: flags for page allocation
 *
 * Allocates memory from an unprotected pool. If the pool doesn't have
 * enough memory, and the request did not include GFP_ATOMIC, an attempt
 * is made to add a new chunk of memory to the pool
 * (a multiple of PAGE_SIZE), in order to fit the new request.
 * Otherwise, NULL is returned.
 *
 * Return:
 * * pointer to the memory requested	- success
 * * NULL				- either no memory available or
 *					  pool already read-only
 */
void *pmalloc(struct gen_pool *pool, size_t size, gfp_t gfp);


/**
 * pzalloc() - zero-initialized version of pmalloc
 * @pool: handle to the pool to be used for memory allocation
 * @size: amount of memory (in bytes) requested
 * @gfp: flags for page allocation
 *
 * Executes pmalloc, initializing the memory requested to 0,
 * before returning the pointer to it.
 *
 * Return:
 * * pointer to the memory requested	- success
 * * NULL				- either no memory available or
 *					  pool already read-only
 */
static inline void *pzalloc(struct gen_pool *pool, size_t size, gfp_t gfp)
{
	return pmalloc(pool, size, gfp | __GFP_ZERO);
}

/**
 * pmalloc_array() - allocates an array according to the parameters
 * @pool: handle to the pool to be used for memory allocation
 * @n: number of elements in the array
 * @size: amount of memory (in bytes) requested for each element
 * @flags: flags for page allocation
 *
 * Executes pmalloc, if it has a chance to succeed.
 *
 * Return:
 * * the pmalloc result	- success
 * * NULL		- error
 */
static inline void *pmalloc_array(struct gen_pool *pool, size_t n,
				  size_t size, gfp_t flags)
{
	if (unlikely(!(pool && n && size)))
		return NULL;
	return pmalloc(pool, n * size, flags);
}

/**
 * pcalloc() - allocates a 0-initialized array according to the parameters
 * @pool: handle to the pool to be used for memory allocation
 * @n: number of elements in the array
 * @size: amount of memory (in bytes) requested
 * @flags: flags for page allocation
 *
 * Executes pmalloc_array, if it has a chance to succeed.
 *
 * Return:
 * * the pmalloc result	- success
 * * NULL		- error
 */
static inline void *pcalloc(struct gen_pool *pool, size_t n,
			    size_t size, gfp_t flags)
{
	return pmalloc_array(pool, n, size, flags | __GFP_ZERO);
}

/**
 * pstrdup() - duplicate a string, using pmalloc as allocator
 * @pool: handle to the pool to be used for memory allocation
 * @s: string to duplicate
 * @gfp: flags for page allocation
 *
 * Generates a copy of the given string, allocating sufficient memory
 * from the given pmalloc pool.
 *
 * Return:
 * * pointer to the replica	- success
 * * NULL			- error
 */
static inline char *pstrdup(struct gen_pool *pool, const char *s, gfp_t gfp)
{
	size_t len;
	char *buf;

	if (unlikely(pool == NULL || s == NULL))
		return NULL;

	len = strlen(s) + 1;
	buf = pmalloc(pool, len, gfp);
	if (likely(buf))
		strncpy(buf, s, len);
	return buf;
}

/**
 * pmalloc_protect_pool() - turn a read/write pool read-only
 * @pool: the pool to protect
 *
 * Write-protects all the memory chunks assigned to the pool.
 * This prevents any further allocation.
 *
 * Return:
 * * 0		- success
 * * -EINVAL	- error
 */
int pmalloc_protect_pool(struct gen_pool *pool);

/**
 * pfree() - mark as unused memory that was previously in use
 * @pool: handle to the pool to be used for memory allocation
 * @addr: the beginning of the memory area to be freed
 *
 * The behavior of pfree is different, depending on the state of the
 * protection.
 * If the pool is not yet protected, the memory is marked as unused and
 * will be available for further allocations.
 * If the pool is already protected, the memory is marked as unused, but
 * it will still be impossible to perform further allocation, because of
 * the existing protection.
 * The freed memory, in this case, will be truly released only when the
 * pool is destroyed.
 */
static inline void pfree(struct gen_pool *pool, const void *addr)
{
	gen_pool_free(pool, (unsigned long)addr, 0);
}

/**
 * pmalloc_destroy_pool() - destroys a pool and all the associated memory
 * @pool: the pool to destroy
 *
 * All the memory that was allocated through pmalloc in the pool will be freed.
 *
 * Return:
 * * 0		- success
 * * -EINVAL	- error
 */
int pmalloc_destroy_pool(struct gen_pool *pool);

#endif
