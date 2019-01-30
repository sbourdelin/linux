/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2019 Intel Corporation
 */

#ifndef _I915_ACTIVE_TYPES_H_
#define _I915_ACTIVE_TYPES_H_

#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>

#include "i915_request.h"

struct i915_active_request;
struct i915_gt_active;
struct kmem_cache;

typedef void (*i915_active_retire_fn)(struct i915_active_request *,
				      struct i915_request *);

struct i915_active_request {
	struct i915_request __rcu *request;
	struct list_head link;
	i915_active_retire_fn retire;
};

struct i915_active {
	struct i915_gt_active *gt;
	struct list_head active_link;

	struct rb_root tree;
	struct i915_active_request last;
	unsigned int count;

	void (*retire)(struct i915_active *ref);
};

struct i915_gt_active {
	struct list_head active_refs;
	struct kmem_cache *slab_cache;
};

#endif /* _I915_ACTIVE_TYPES_H_ */
