/* SPDX-License-Identifier: GPL-2.0 */
/*
 * sparsemask.h - sparse bitmap operations
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

#ifndef __LINUX_SPARSEMASK_H
#define __LINUX_SPARSEMASK_H

#include <linux/kernel.h>
#include <linux/bitmap.h>
#include <linux/bug.h>

/*
 * A sparsemask is a sparse bitmap.  It reduces cache contention vs the usual
 * bitmap when many threads concurrently set, clear, and visit elements.  For
 * each 64 byte chunk of the mask, only the first K bits of the first word are
 * used, and the remaining bits are ignored, where K is a creation time
 * parameter.  Thus a sparsemask that can represent a set of N elements is
 * approximately (N/K * 64) bytes in size.
 *
 * Clients pass and receive element numbers in the public API, and the
 * implementation translates them to bit numbers to perform the bitmap
 * operations.
 *
 * This file is partially derived from cpumask.h, and the public sparsemask
 * operations are drop-in replacements for cpumask operations. However,
 * sparsemask has no dependency on CPU definitions and can be used to
 * represent any kind of elements.
 */

struct sparsemask {
	short nelems;		/* current number of elements */
	short density;		/* store 2^density elements per chunk */
	unsigned long bits[0];	/* embedded array of chunks */
};

/* The maximum value for density, which implicitly defines the chunk size */

#define _SMASK_DENSITY_MAX	6

#define SMASK_DENSITY_TO_BYTES(density)		(1U << (density))
#define SMASK_DENSITY_TO_ELEMS(density)		(1U << (density))

/* The number of elements/bits/bytes/longs in a chunk */

#define SMASK_ELEMS(mask)	SMASK_DENSITY_TO_ELEMS((mask)->density)
#define SMASK_BYTES		SMASK_DENSITY_TO_BYTES(_SMASK_DENSITY_MAX)
#define SMASK_BITS		(SMASK_BYTES * BITS_PER_BYTE)
#define SMASK_LONGS		(SMASK_BYTES / sizeof(long))

/*
 * Translate element index @elem to a bit/byte/long index.
 * @density: the density of a chunk.
 */

#define _SMASK_ELEM_TO_BIT(elem, density)			\
	((elem) / SMASK_DENSITY_TO_ELEMS(density) * SMASK_BITS +\
	 (elem) % SMASK_DENSITY_TO_ELEMS(density))

#define _SMASK_ELEM_TO_BYTE(elem, density)	\
	(_SMASK_ELEM_TO_BIT(elem, density) / BITS_PER_BYTE)

#define _SMASK_ELEM_TO_LONG(elem, density)	\
	(_SMASK_ELEM_TO_BYTE(elem, density) / sizeof(long))

/* Translate @bit/@byte/@long index to an element index */

#define _SMASK_BIT_TO_ELEM(bit, density)			\
	((bit) / SMASK_BITS * SMASK_DENSITY_TO_ELEMS(density) +	\
	 (bit) % SMASK_BITS)

#define _SMASK_BYTE_TO_ELEM(byte, density)	\
	_SMASK_BIT_TO_ELEM((byte) * BITS_PER_BYTE, density)

#define _SMASK_LONG_TO_ELEM(index, density)	\
	_SMASK_BYTE_TO_ELEM((index) * sizeof(long), density)

/* Same translations as above, but taking sparsemask @m instead of density */

#define SMASK_ELEM_TO_BYTE(elem, m)	_SMASK_ELEM_TO_BYTE(elem, (m)->density)
#define SMASK_ELEM_TO_BIT(elem, m)	_SMASK_ELEM_TO_BIT(elem, (m)->density)
#define SMASK_ELEM_TO_LONG(elem, m)	_SMASK_ELEM_TO_LONG(elem, (m)->density)
#define SMASK_BYTE_TO_ELEM(byte, m)	_SMASK_BYTE_TO_ELEM(byte, (m)->density)
#define SMASK_BIT_TO_ELEM(bit, m)	_SMASK_BIT_TO_ELEM(bit, (m)->density)
#define SMASK_LONG_TO_ELEM(index, m)	_SMASK_LONG_TO_ELEM(index, (m)->density)

/*
 * Verify the @elem argument to sparsemask functions, and return its bit.
 */
static inline int
sparsemask_check(int elem, const struct sparsemask *mask)
{
	WARN_ON_ONCE(elem >= mask->nelems);
	return SMASK_ELEM_TO_BIT(elem, mask);
}

int sparsemask_next(int n, const struct sparsemask *srcp);
int sparsemask_next_wrap(int n, const struct sparsemask *mask,
			 int start, bool wrap);

/****************** The public API ********************/

/*
 * for_each_sparse - iterate over every element in a mask
 * @elem: the (optionally unsigned) integer iterator
 * @mask: the sparsemask
 *
 * After the loop, @elem is >= @mask->nelems.
 */
#define for_each_sparse(elem, mask)			\
	for ((elem) = -1;				\
	     (elem) = sparsemask_next((elem), (mask)),	\
	     (elem) < (mask)->nelems;)

/*
 * for_each_sparse_wrap - iterate over every element in a mask, starting at a
 *   specified location.
 * @elem: the (optionally unsigned) integer iterator
 * @mask: the sparsemask
 * @start: the start location
 *
 * The implementation does not assume any bit in @mask is set(including @start).
 * After the loop, @elem is >= @mask->nelems.
 */
#define for_each_sparse_wrap(elem, mask, start)				       \
	for ((elem) = sparsemask_next_wrap((start)-1, (mask), (start), false); \
	     (elem) < (mask)->nelems;					       \
	     (elem) = sparsemask_next_wrap((elem), (mask), (start), true))

/*
 * sparsemask_set_elem - set an element in a sparsemask
 * @elem: element number (< @dstp->nelems)
 * @dstp: the sparsemask
 */
static inline void sparsemask_set_elem(int elem, struct sparsemask *dstp)
{
	set_bit(sparsemask_check(elem, dstp), dstp->bits);
}

static inline void __sparsemask_set_elem(int elem, struct sparsemask *dstp)
{
	__set_bit(sparsemask_check(elem, dstp), dstp->bits);
}

/*
 * sparsemask_clear_elem - clear an element in a sparsemask
 * @elem: element number (< @dstp->nelems)
 * @dstp: the sparsemask
 */
static inline void sparsemask_clear_elem(int elem, struct sparsemask *dstp)
{
	clear_bit(sparsemask_check(elem, dstp), dstp->bits);
}

static inline void __sparsemask_clear_elem(int elem, struct sparsemask *dstp)
{
	__clear_bit(sparsemask_check(elem, dstp), dstp->bits);
}

/*
 * sparsemask_test_elem - test for an element in a sparsemask
 * @elem: element number (< @mask->nelems)
 * @mask: the sparsemask
 *
 * Returns 1 if @elem is set in @mask, else returns 0
 */
static inline int sparsemask_test_elem(int elem, const struct sparsemask *mask)
{
	return test_bit(sparsemask_check(elem, mask), mask->bits);
}

/*
 * sparsemask_test_and_set_elem - atomically test and set an element
 * @elem: element number (< @mask->nelems)
 * @mask: the sparsemask
 *
 * Returns 1 if @elem is set in old bitmap of @mask, else returns 0
 */
static inline int
sparsemask_test_and_set_elem(int elem, struct sparsemask *mask)
{
	return test_and_set_bit(sparsemask_check(elem, mask), mask->bits);
}

/*
 * sparsemask_test_and_clear_elem - atomically test and clear an element
 * @elem: element number (< @mask->nelems)
 * @mask: the sparsemask
 *
 * Returns 1 if @elem is set in old bitmap of @mask, else returns 0
 */
static inline int
sparsemask_test_and_clear_elem(int elem, struct sparsemask *mask)
{
	return test_and_clear_bit(sparsemask_check(elem, mask), mask->bits);
}

/*
 * sparsemask_weight - return count of bits in @mask (<= @mask->nelems)
 * @mask: the sparsemask
 */
unsigned int sparsemask_weight(const struct sparsemask *srcp);

/*
 * Suggested and max value for the density parameter
 */
#define SPARSEMASK_DENSITY_DEFAULT	3
#define SMASK_DENSITY_MAX		_SMASK_DENSITY_MAX

/*
 * Allocate and initialize a sparsemask and return it in @maskp.
 * @nelems - maximum number of elements.
 * @density - store 2^density elements per 64-byte chunk.
 *	      values from 0 to SMASK_DENSITY_MAX inclusive.
 * @flags - kmalloc allocation flags
 * @node - numa node
 *
 * Return true on success, like the cpumask functions.
 */

bool alloc_sparsemask(struct sparsemask **maskp, int nelems,
		      int density, gfp_t flags);

bool zalloc_sparsemask(struct sparsemask **maskp, int nelems,
		       int density, gfp_t flags);

bool alloc_sparsemask_node(struct sparsemask **maskp, int nelems,
			   int density, gfp_t flags, int node);

bool zalloc_sparsemask_node(struct sparsemask **maskp, int nelems,
			    int density, gfp_t flags, int node);

/*
 * Free a sparsemask allocated by any of the above
 */
void free_sparsemask(struct sparsemask *mask);

/*
 * Return bytes to allocate for a sparsemask, for custom allocators
 */
size_t sparsemask_size(int nelems, int density);

/*
 * Initialize an allocated sparsemask, for custom allocators
 */
void sparsemask_init(struct sparsemask *mask, int nelems, int density);

#endif /* __LINUX_SPARSEMASK_H */
