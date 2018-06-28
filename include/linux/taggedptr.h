/* SPDX-License-Identifier: GPL-2.0
 *
 * Tagged pointer implementation
 *
 * Copyright (C) 2018 Gao Xiang <gaoxiang25@huawei.com>
 */
#ifndef _LINUX_TAGGEDPTR_H
#define _LINUX_TAGGEDPTR_H

#include <linux/types.h>
#include <linux/build_bug.h>

/*
 * mark these special integers as another type
 * in order to highlight the tagged pointer usage.
 */
typedef uintptr_t	taggedptr_t;

/*
 * generally for all architectures, the last 2 bits of
 * pointer can be used safely
 */
#ifndef TAGGEDPTR_TAGS_BITS
#define TAGGEDPTR_TAGS_BITS	2
#endif

#define TAGGEDPTR_TAGS_MASK	((1 << TAGGEDPTR_TAGS_BITS) - 1)

extern void __compiletime_error("bad taggedptr tags")
	__bad_taggedptr_tags(void);

/* encode the tagged pointer */
static inline taggedptr_t taggedptr_fold(void *ptr, unsigned int tags)
{
	if (__builtin_constant_p(tags) && (tags & ~TAGGEDPTR_TAGS_MASK))
		__bad_taggedptr_tags();

	return (taggedptr_t)ptr | tags;
}

static inline void *taggedptr_unfold_ptr(taggedptr_t tptr)
{
	return (void *)(tptr & ~TAGGEDPTR_TAGS_MASK);
}

static inline unsigned int taggedptr_unfold_tags(taggedptr_t tptr)
{
	return tptr & TAGGEDPTR_TAGS_MASK;
}

static inline taggedptr_t taggedptr_replace_tags(taggedptr_t tptr,
						 unsigned int tags)
{
	return taggedptr_fold(taggedptr_unfold_ptr(tptr), tags);
}

static inline taggedptr_t taggedptr_set_tags(taggedptr_t tptr,
					     unsigned int tags)
{
	if (__builtin_constant_p(tags) && (tags & ~TAGGEDPTR_TAGS_MASK))
		__bad_taggedptr_tags();

	return tptr |= tags;
}

static inline taggedptr_t taggedptr_clear_tags(taggedptr_t tptr,
					       unsigned int tags)
{
	if (__builtin_constant_p(tags) && (tags & ~TAGGEDPTR_TAGS_MASK))
		__bad_taggedptr_tags();

	return tptr &= ~tags;
}

#endif

