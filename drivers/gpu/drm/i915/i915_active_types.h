/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef _I915_ACTIVE_TYPES_H_
#define _I915_ACTIVE_TYPES_H_

#include <linux/list.h>
#include <linux/rbtree.h>

#include "i915_request.h"

struct i915_gt_active;
struct kmem_cache;

struct i915_active {
	struct i915_gt_active *gt;
	struct list_head active_link;

	struct rb_root tree;
	struct i915_gem_active last;
	unsigned int count;

	void (*retire)(struct i915_active *ref);
};

struct i915_gt_active {
	struct list_head active_refs;
	struct kmem_cache *slab_cache;
};

#endif /* _I915_ACTIVE_TYPES_H_ */
