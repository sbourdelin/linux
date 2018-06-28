/* SPDX-License-Identifier: GPL-2.0
 *
 * Tagged pointer implementation
 *
 * Copyright (C) 2018 Gao Xiang <gaoxiang25@huawei.com>
 */
#ifndef _LINUX_TAGPTR_H
#define _LINUX_TAGPTR_H

#include <linux/types.h>
#include <linux/build_bug.h>

/* the name of types are tagptr{1, 2, 3...}_t */
#define __MAKE_TAGPTR(n) \
typedef struct tagptr##n {	\
	uintptr_t v;	\
} tagptr##n##_t;

__MAKE_TAGPTR(1)
__MAKE_TAGPTR(2)
__MAKE_TAGPTR(3)
__MAKE_TAGPTR(4)

#undef __MAKE_TAGPTR

extern void __compiletime_error("bad tagptr tags")
	__bad_tagptr_tags(void);

extern void __compiletime_error("bad tagptr type")
	__bad_tagptr_type(void);

#define __tagptr_mask_1(ptr, n)	\
	__builtin_types_compatible_p(typeof(ptr), tagptr##n##_t) ? \
		(1UL << (n)) - 1 :

#define __tagptr_mask(ptr)	(\
	__tagptr_mask_1(ptr, 1) ( \
	__tagptr_mask_1(ptr, 2) ( \
	__tagptr_mask_1(ptr, 3) ( \
	__tagptr_mask_1(ptr, 4) ( \
	__bad_tagptr_type(), 0)))))

/* encode the tagged pointer */
#define tagptr_fold(type, ptr, _tags) ({ \
	const typeof(_tags) tags = (_tags); \
	if (__builtin_constant_p(tags) && (tags & ~__tagptr_mask(type))) \
		__bad_tagptr_tags(); \
((typeof(type)){ .v = (uintptr_t)ptr | tags }); })

#define tagptr_unfold_ptr(tptr) \
	((void *)((tptr).v & ~__tagptr_mask(tptr)))

#define tagptr_unfold_tags(tptr) \
	((tptr).v & __tagptr_mask(tptr))

#define tagptr_replace_tags(_ptptr, tags) ({ \
	typeof(_ptptr) ptptr = (_ptptr); \
	*ptptr = tagptr_fold(*ptptr, tagptr_unfold_ptr(*ptptr), tags); \
*ptptr; })

#define tagptr_set_tags(_ptptr, _tags) ({ \
	typeof(_ptptr) ptptr = (_ptptr); \
	const typeof(_tags) tags = (_tags); \
	if (__builtin_constant_p(tags) && (tags & ~__tagptr_mask(*ptptr))) \
		__bad_tagptr_tags(); \
	ptptr->v |= tags; \
*ptptr; })

#define tagptr_clear_tags(_ptptr, _tags) ({ \
	typeof(_ptptr) ptptr = (_ptptr); \
	const typeof(_tags) tags = (_tags); \
	if (__builtin_constant_p(tags) && (tags & ~__tagptr_mask(*ptptr))) \
		__bad_tagptr_tags(); \
	ptptr->v &= ~tags; \
*ptptr; })

#endif

