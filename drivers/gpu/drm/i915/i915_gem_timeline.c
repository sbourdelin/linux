/*
 * Copyright © 2016 Intel Corporation
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

#define SHIFT ilog2(NSEQMAP)
#define MASK (NSEQMAP - 1)

static void seqmap_free_layers(struct seqmap_layer *p)
{
	unsigned int i;

	if (p->height) {
		for (; (i = ffs(p->bitmap)); p->bitmap &= ~0u << i)
			seqmap_free_layers(p->slot[i - 1]);
	}

	kfree(p);
}

static void seqmap_free(struct seqmap *seqmap)
{
	if (seqmap->top)
		seqmap_free_layers(seqmap->top);

	while (seqmap->freed) {
		struct seqmap_layer *p;

		p = ptr_mask_bits(seqmap->freed, SEQMAP_COUNT_BITS);
		seqmap->freed = p->parent;
		kfree(p);
	}
}

__malloc static struct seqmap_layer *
seqmap_alloc_layer(struct seqmap *shared)
{
	struct seqmap_layer *p;

	GEM_BUG_ON(!shared->freed);

	p = ptr_mask_bits(shared->freed, SEQMAP_COUNT_BITS);
	shared->freed = p->parent;

	return p;
}

static int layer_idx(const struct seqmap_layer *p, u64 id)
{
	return (id >> p->height) & MASK;
}

bool intel_timeline_sync_get(struct intel_timeline *tl, u64 id, u32 seqno)
{
	struct seqmap *shared = &tl->sync;
	struct seqmap_layer *p;
	unsigned int idx;

	p = shared->hint;
	if (!p)
		return false;

	if ((id >> SHIFT) == p->prefix)
		goto found;

	p = shared->top;
	do {
		if ((id >> p->height >> SHIFT) != p->prefix)
			return false;

		if (!p->height)
			break;

		p = p->slot[layer_idx(p, id)];
		if (!p)
			return false;
	} while (1);

	shared->hint = p;
found:
	idx = id & MASK;
	if (!(p->bitmap & BIT(idx)))
		return false;

	return i915_seqno_passed((uintptr_t)p->slot[idx], seqno);
}

void intel_timeline_sync_set(struct intel_timeline *tl, u64 id, u32 seqno)
{
	struct seqmap *shared = &tl->sync;
	struct seqmap_layer *p, *cur;
	unsigned int idx;

	/* We expect to be called in sequence following a  _get(id), which
	 * should have preloaded the ->hint for us.
	 */
	p = shared->hint;
	if (likely(p && (id >> SHIFT) == p->prefix))
		goto found_layer;

	if (!p) {
		GEM_BUG_ON(shared->top);
		cur = seqmap_alloc_layer(shared);
		cur->parent = NULL;
		shared->top = cur;
		goto new_layer;
	}

	/* No shortcut, we have to descend the tree to find the right layer
	 * containing this fence.
	 *
	 * Each layer in the tree holds 16 (NSEQMAP) pointers, either fences
	 * or lower layers. Leaf nodes (height = 0) contain the fences, all
	 * other nodes (height > 0) are internal layers that point to a lower
	 * node. Each internal layer has at least 2 descendents.
	 *
	 * Starting at the top, we check whether the current prefix matches. If
	 * it doesn't, we have gone passed our layer and need to insert a join
	 * into the tree, and a new leaf node as a descendent as well as the
	 * original layer.
	 *
	 * The matching prefix means we are still following the right branch
	 * of the tree. If it has height 0, we have found our leaf and just
	 * need to replace the fence slot with ourselves. If the height is
	 * not zero, our slot contains the next layer in the tree (unless
	 * it is empty, in which case we can add ourselves as a new leaf).
	 * As descend the tree the prefix grows (and height decreases).
	 */
	p = shared->top;
	do {
		if ((id >> p->height >> SHIFT) != p->prefix) {
			/* insert a join above the current layer */
			cur = seqmap_alloc_layer(shared);
			cur->height = ALIGN(fls64((id >> p->height >> SHIFT) ^ p->prefix),
					    SHIFT) + p->height;
			cur->prefix = id >> cur->height >> SHIFT;

			if (p->parent)
				p->parent->slot[layer_idx(p->parent, id)] = cur;
			else
				shared->top = cur;
			cur->parent = p->parent;

			idx = p->prefix >> (cur->height - p->height - SHIFT) & MASK;
			cur->slot[idx] = p;
			cur->bitmap |= BIT(idx);
			p->parent = cur;
		} else if (!p->height) {
			/* matching base layer */
			shared->hint = p;
			goto found_layer;
		} else {
			/* descend into the next layer */
			idx = layer_idx(p, id);
			cur = p->slot[idx];
			if (unlikely(!cur)) {
				cur = seqmap_alloc_layer(shared);
				p->slot[idx] = cur;
				p->bitmap |= BIT(idx);
				cur->parent = p;
				goto new_layer;
			}
		}

		p = cur;
	} while (1);

found_layer:
	GEM_BUG_ON(p->height);
	GEM_BUG_ON(p->prefix != id >> SHIFT);
	idx = id & MASK;
	p->slot[idx] = (void *)(uintptr_t)seqno;
	p->bitmap |= BIT(idx);
	GEM_BUG_ON(shared->hint != p);
	return;

new_layer:
	GEM_BUG_ON(cur->height);
	cur->prefix = id >> SHIFT;
	idx = id & MASK;
	cur->slot[idx] = (void *)(uintptr_t)seqno;
	cur->bitmap = BIT(idx);
	shared->hint = cur;
	return;
}

int __intel_timeline_sync_reserve(struct intel_timeline *tl)
{
	struct seqmap *shared = &tl->sync;
	int count;

	might_sleep();

	/* To guarantee being able to replace a fence in the radixtree,
	 * we need at most 2 layers: one to create a join in the tree,
	 * and one to contain the fence. Typically we expect to reuse
	 * a layer and so avoid any insertions.
	 *
	 * We use the low bits of the freed list to track its length
	 * since we only need a couple of bits.
	 */
	count = ptr_unmask_bits(shared->freed, SEQMAP_COUNT_BITS);
	while (count++ < 2) {
		struct seqmap_layer *p;

		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (unlikely(!p))
			return -ENOMEM;

		p->parent = shared->freed;
		shared->freed = ptr_pack_bits(p, count, SEQMAP_COUNT_BITS);
	}

	return 0;
}

static int __i915_gem_timeline_init(struct drm_i915_private *i915,
				    struct i915_gem_timeline *timeline,
				    const char *name,
				    struct lock_class_key *lockclass,
				    const char *lockname)
{
	unsigned int i;
	u64 fences;

	lockdep_assert_held(&i915->drm.struct_mutex);

	timeline->i915 = i915;
	timeline->name = kstrdup(name ?: "[kernel]", GFP_KERNEL);
	if (!timeline->name)
		return -ENOMEM;

	list_add(&timeline->link, &i915->gt.timelines);

	/* Called during early_init before we know how many engines there are */
	fences = dma_fence_context_alloc(ARRAY_SIZE(timeline->engine));
	for (i = 0; i < ARRAY_SIZE(timeline->engine); i++) {
		struct intel_timeline *tl = &timeline->engine[i];

		tl->fence_context = fences++;
		tl->common = timeline;
#ifdef CONFIG_DEBUG_SPINLOCK
		__raw_spin_lock_init(&tl->lock.rlock, lockname, lockclass);
#else
		spin_lock_init(&tl->lock);
#endif
		init_request_active(&tl->last_request, NULL);
		INIT_LIST_HEAD(&tl->requests);
	}

	return 0;
}

int i915_gem_timeline_init(struct drm_i915_private *i915,
			   struct i915_gem_timeline *timeline,
			   const char *name)
{
	static struct lock_class_key class;

	return __i915_gem_timeline_init(i915, timeline, name,
					&class, "&timeline->lock");
}

int i915_gem_timeline_init__global(struct drm_i915_private *i915)
{
	static struct lock_class_key class;

	return __i915_gem_timeline_init(i915,
					&i915->gt.global_timeline,
					"[execution]",
					&class, "&global_timeline->lock");
}

void i915_gem_timeline_fini(struct i915_gem_timeline *timeline)
{
	int i;

	lockdep_assert_held(&timeline->i915->drm.struct_mutex);

	for (i = 0; i < ARRAY_SIZE(timeline->engine); i++) {
		struct intel_timeline *tl = &timeline->engine[i];

		GEM_BUG_ON(!list_empty(&tl->requests));

		seqmap_free(&tl->sync);
	}

	list_del(&timeline->link);
	kfree(timeline->name);
}

#if IS_ENABLED(CONFIG_DRM_I915_SELFTEST)
#include "selftests/i915_gem_timeline.c"
#endif
