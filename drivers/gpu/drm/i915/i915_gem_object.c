/*
 * Copyright © 2017 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include "i915_drv.h"
#include "i915_gem_object.h"

#if IS_ENABLED(CONFIG_DRM_I915_DEBUG_GEM)

#include <linux/sort.h>

#define STACKDEPTH 12

void track_i915_gem_object_pin_pages(struct drm_i915_gem_object *obj)
{
	unsigned long entries[STACKDEPTH];
	struct stack_trace trace = {
		.entries = entries,
		.max_entries = ARRAY_SIZE(entries),
		.skip = 1
	};
	unsigned long flags;
	depot_stack_handle_t stack, *stacks;

	save_stack_trace(&trace);
	if (trace.nr_entries &&
	    trace.entries[trace.nr_entries - 1] == ULONG_MAX)
		trace.nr_entries--;

	stack = depot_save_stack(&trace, GFP_KERNEL | __GFP_NOWARN);
	if (!stack)
		return;

	spin_lock_irqsave(&obj->mm.debug_lock, flags);
	stacks = krealloc(obj->mm.debug_owners,
			  (obj->mm.debug_count + 1) * sizeof(*stacks),
			  GFP_NOWAIT | __GFP_NOWARN);
	if (stacks) {
		stacks[obj->mm.debug_count++] = stack;
		obj->mm.debug_owners = stacks;
	}
	spin_unlock_irqrestore(&obj->mm.debug_lock, flags);
}

void untrack_i915_gem_object_pin_pages(struct drm_i915_gem_object *obj)
{
	depot_stack_handle_t *stacks;
	unsigned long flags;

	spin_lock_irqsave(&obj->mm.debug_lock, flags);
	stacks = fetch_and_zero(&obj->mm.debug_owners);
	obj->mm.debug_count = 0;
	spin_unlock_irqrestore(&obj->mm.debug_lock, flags);

	kfree(stacks);
}

static int cmphandle(const void *_a, const void *_b)
{
	const depot_stack_handle_t * const a = _a, * const b = _b;

	if (*a < *b)
		return -1;
	else if (*a > *b)
		return 1;
	else
		return 0;
}

void show_i915_gem_object_pin_pages(struct drm_i915_gem_object *obj)
{
	unsigned long entries[STACKDEPTH];
	depot_stack_handle_t *stacks;
	unsigned long flags, count, i;
	char *buf;

	spin_lock_irqsave(&obj->mm.debug_lock, flags);
	stacks = fetch_and_zero(&obj->mm.debug_owners);
	count = fetch_and_zero(&obj->mm.debug_count);
	spin_unlock_irqrestore(&obj->mm.debug_lock, flags);
	if (!count)
		return;

	DRM_DEBUG_DRIVER("obj %p leaked pages, pinned %lu\n", obj, count);

	buf = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!buf)
		goto out_stacks;

	sort(stacks, count, sizeof(*stacks), cmphandle, NULL);

	for (i = 0; i < count; i++) {
		struct stack_trace trace = {
			.entries = entries,
			.max_entries = ARRAY_SIZE(entries),
		};
		depot_stack_handle_t stack = stacks[i];
		unsigned long rep;

		rep = 1;
		while (i + 1 < count && stacks[i + 1] == stack)
			rep++, i++;

		depot_fetch_stack(stack, &trace);
		snprint_stack_trace(buf, PAGE_SIZE, &trace, 0);
		DRM_DEBUG_DRIVER("obj %p pages pinned x%lu at\n%s",
				 obj, rep, buf);
	}

	kfree(buf);
out_stacks:
	kfree(stacks);
}

#endif

/**
 * Mark up the object's coherency levels for a given cache_level
 * @obj: #drm_i915_gem_object
 * @cache_level: cache level
 */
void i915_gem_object_set_cache_coherency(struct drm_i915_gem_object *obj,
					 unsigned int cache_level)
{
	obj->cache_level = cache_level;

	if (cache_level != I915_CACHE_NONE)
		obj->cache_coherent = (I915_BO_CACHE_COHERENT_FOR_READ |
				       I915_BO_CACHE_COHERENT_FOR_WRITE);
	else if (HAS_LLC(to_i915(obj->base.dev)))
		obj->cache_coherent = I915_BO_CACHE_COHERENT_FOR_READ;
	else
		obj->cache_coherent = 0;

	obj->cache_dirty =
		!(obj->cache_coherent & I915_BO_CACHE_COHERENT_FOR_WRITE);
}
