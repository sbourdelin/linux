/*
 * Copyright (C) 2012-2014 Canonical Ltd (Maarten Lankhorst)
 *
 * Based on bo.c which bears the following copyright notice,
 * but is dual licensed:
 *
 * Copyright (c) 2006-2009 VMware, Inc., Palo Alto, CA., USA
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/
/*
 * Authors: Thomas Hellstrom <thellstrom-at-vmware-dot-com>
 */

#include <linux/reservation.h>
#include <linux/export.h>

/**
 * DOC: Reservation Object Overview
 *
 * The reservation object provides a mechanism to manage shared and
 * exclusive fences associated with a buffer.  A reservation object
 * can have attached one exclusive fence (normally associated with
 * write operations) or N shared fences (read operations).  The RCU
 * mechanism is used to protect read access to fences from locked
 * write-side updates.
 */

#if 0
#define assert(x) BUG_ON(!(x))
#define dbg(x) pr_err x
#else
#define assert(x) do { } while (0)
#define dbg(x) do { } while (0)
#endif

DEFINE_WW_CLASS(reservation_ww_class);
EXPORT_SYMBOL(reservation_ww_class);

struct lock_class_key reservation_seqcount_class;
EXPORT_SYMBOL(reservation_seqcount_class);

const char reservation_seqcount_string[] = "reservation_seqcount";
EXPORT_SYMBOL(reservation_seqcount_string);

#define SHIFT ilog2(NSHARED)
#define MASK (NSHARED - 1)

void __reservation_shared_iter_next(struct reservation_shared_iter *iter)
{
	struct reservation_shared_layer *p = iter->p;
	int pos, h;

	do {
		p = p->parent;
		if (!p) {
			iter->fence = NULL;
			return;
		}

		h = p->height / SHIFT;
		pos = fns(p->bitmap, iter->stack[h] + 1);
	} while (!pos);

	iter->stack[h] = --pos;

	__reservation_shared_iter_fill(iter, p->slot[pos]);
}
EXPORT_SYMBOL(__reservation_shared_iter_next);

#define ptr_mask(ptr) (__alignof__(*(ptr)) - 1)

#define ptr_mask_bits(ptr) ({						\
	unsigned long __v = (unsigned long)(ptr);			\
	(typeof(ptr))(__v & ~ptr_mask(ptr));				\
})

#define ptr_get_bits(ptr) ({						\
	unsigned long __v = (unsigned long)(ptr);			\
	(__v & ptr_mask(ptr));						\
})

#define ptr_set_bits(ptr, x) ({						\
	unsigned long __v = (unsigned long)(ptr);			\
	(typeof(ptr))(__v | (x));					\
})

static void shared_free_layers(struct reservation_shared_layer *p)
{
	unsigned int i;

	if (p->height) {
		for (; (i = ffs(p->bitmap)); p->bitmap &= ~0 << i)
			shared_free_layers(p->slot[i - 1]);
	} else {
		for (; (i = ffs(p->bitmap)); p->bitmap &= ~0 << i)
			dma_fence_put(p->slot[i - 1]);
	}

	/* Defer the free until after any concurrent readers finish. */
	p->parent = NULL;
	kfree_rcu(p, rcu);
}

static void shared_free(struct reservation_shared *shared)
{
	if (!shared->top)
		return;

	shared_free_layers(shared->top);
}

void reservation_shared_destroy(struct reservation_shared *shared)
{
	struct reservation_shared_layer *p;

	shared_free(shared);

	while (shared->freed) {
		p = ptr_mask_bits(shared->freed);
		shared->freed = p->parent;
		kfree(p);
	}
}
EXPORT_SYMBOL(reservation_shared_destroy);

__malloc static struct reservation_shared_layer *
shared_alloc_layer(struct reservation_shared *shared)
{
	struct reservation_shared_layer *p;

	p = ptr_mask_bits(shared->freed);
	shared->freed = p->parent;

	return p;
}

static int layer_idx(const struct reservation_shared_layer *p, u64 id)
{
	return (id >> p->height) & MASK;
}

static void *
shared_fence_replace(struct reservation_shared *shared, u64 id, void *item)
{
	struct reservation_shared_layer *p, *cur;
	unsigned int idx;

	dbg(("%s(id=%llx)\n", __func__, id));

	/* First see if this fence is in the same layer as the previous fence */
	p = shared->hint;
	if (p && (id >> SHIFT) == p->prefix) {
		assert(p->height == 0);
		goto found_layer;
	}

	p = shared->top;
	if (!p) {
		cur = shared_alloc_layer(shared);
		cur->parent = NULL;
		shared->top = cur;
		goto new_layer;
	}

	/* No shortcut, we have to descend the tree to find the right layer
	 * containing this fence.
	 *
	 * Each layer in the tree holds 16 (NSHARED) pointers, either fences
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
	do {
		dbg(("id=%llx, p->(prefix=%llx, height=%d), delta=%llx, idx=%x\n",
		     id, p->prefix, p->height,
		     id >> p->height >> SHIFT,
		     layer_idx(p, id)));

		if ((id >> p->height >> SHIFT) != p->prefix) {
			/* insert a join above the current layer */
			cur = shared_alloc_layer(shared);
			cur->height = ALIGN(fls64((id >> p->height >> SHIFT) ^ p->prefix),
					    SHIFT) + p->height;
			cur->prefix = id >> cur->height >> SHIFT;

			dbg(("id=%llx, join prefix=%llu, height=%d\n",
			     id, cur->prefix, cur->height));

			assert((id >> cur->height >> SHIFT) == cur->prefix);
			assert(cur->height > p->height);

			if (p->parent) {
				assert(p->parent->slot[layer_idx(p->parent, id)] == p);
				assert(cur->height < p->parent->height);
				p->parent->slot[layer_idx(p->parent, id)] = cur;
			} else {
				shared->top = cur;
			}
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
				cur = shared_alloc_layer(shared);
				p->slot[idx] = cur;
				p->bitmap |= BIT(idx);
				cur->parent = p;
				goto new_layer;
			}
		}

		p = cur;
	} while (1);

found_layer:
	assert(p->height == 0);
	idx = id & MASK;
	cur = p->slot[idx];
	p->slot[idx] = item;
	p->bitmap |= BIT(idx);
	dbg(("id=%llx, found existing layer prefix=%llx, idx=%x [bitmap=%x]\n",
	     id, p->prefix, idx, p->bitmap));
	return (void *)cur;

new_layer:
	assert(cur->height == 0);
	assert(cur->bitmap == 0);
	cur->prefix = id >> SHIFT;
	cur->slot[id & MASK] = item;
	cur->bitmap = BIT(id & MASK);
	shared->hint = cur;
	dbg(("id=%llx, new layer prefix=%llx, idx=%x [bitmap=%x]\n",
	     id, cur->prefix, (int)(id & MASK), cur->bitmap));
	return NULL;
}

/**
 * reservation_object_reserve_shared - Reserve space to add a shared
 * fence to a reservation_object.
 * @obj: reservation object
 *
 * Should be called before reservation_object_add_shared_fence().  Must
 * be called with obj->lock held.
 *
 * RETURNS
 * Zero for success, or -errno
 */
int reservation_object_reserve_shared(struct reservation_object *obj)
{
	struct reservation_shared *shared = &obj->shared;
	int count;

	reservation_object_assert_held(obj);
	might_sleep();

	/* To guarantee being able to replace a fence in the radixtree,
	 * we need at most 2 layers: one to create a join in the tree,
	 * and one to contain the fence. Typically we expect to reuse
	 * a layer and so avoid any insertions.
	 *
	 * We use the low bits of the freed list to track its length
	 * since we only need a couple of bits.
	 */
	count = ptr_get_bits(shared->freed);
	while (count++ < 2) {
		struct reservation_shared_layer *p;

		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (unlikely(!p))
			return -ENOMEM;

		p->parent = shared->freed;
		shared->freed = ptr_set_bits(p, count);
	}

	return 0;
}
EXPORT_SYMBOL(reservation_object_reserve_shared);

/**
 * reservation_object_add_shared_fence - Add a fence to a shared slot
 * @obj: the reservation object
 * @fence: the shared fence to add
 *
 * Add a fence to a shared slot, obj->lock must be held, and
 * reservation_object_reserve_shared() has been called.
 */
void reservation_object_add_shared_fence(struct reservation_object *obj,
					 struct dma_fence *fence)
{
	struct dma_fence *old_fence;

	reservation_object_assert_held(obj);

	dma_fence_get(fence);

	preempt_disable();
	write_seqcount_begin(&obj->seq);

	old_fence = shared_fence_replace(&obj->shared, fence->context, fence);

	write_seqcount_end(&obj->seq);
	preempt_enable();

	dma_fence_put(old_fence);
}
EXPORT_SYMBOL(reservation_object_add_shared_fence);

/**
 * reservation_object_add_excl_fence - Add an exclusive fence.
 * @obj: the reservation object
 * @fence: the shared fence to add
 *
 * Add a fence to the exclusive slot.  The obj->lock must be held.
 */
void reservation_object_add_excl_fence(struct reservation_object *obj,
				       struct dma_fence *fence)
{
	struct dma_fence *old_fence = obj->excl;
	struct reservation_shared old_shared = obj->shared;

	reservation_object_assert_held(obj);

	dma_fence_get(fence);

	preempt_disable();
	write_seqcount_begin(&obj->seq);

	/* write_seqcount_begin provides the necessary memory barrier */
	RCU_INIT_POINTER(obj->excl, fence);
	reservation_shared_init(&obj->shared);

	write_seqcount_end(&obj->seq);
	preempt_enable();

	shared_free(&old_shared);
	obj->shared.freed = old_shared.freed;

	dma_fence_put(old_fence);
}
EXPORT_SYMBOL(reservation_object_add_excl_fence);

/**
 * reservation_object_get_fences_rcu - Get an object's shared and exclusive
 * fences without update side lock held
 * @obj: the reservation object
 * @pfence_excl: the returned exclusive fence (or NULL)
 * @pshared_count: the number of shared fences returned
 * @pshared: the array of shared fence ptrs returned (array is krealloc'd to
 * the required size, and must be freed by caller)
 *
 * RETURNS
 * Zero or -errno
 */
int reservation_object_get_fences_rcu(struct reservation_object *obj,
				      struct dma_fence **pfence_excl,
				      unsigned *pshared_count,
				      struct dma_fence ***pshared)
{
	struct dma_fence **shared = NULL, *excl;
	unsigned int sz = 0, count;

	rcu_read_lock();
	for (;;) {
		struct reservation_shared_iter iter;
		unsigned int seq;

restart:
		seq = read_seqcount_begin(&obj->seq);

		excl = rcu_dereference(obj->excl);
		if (excl && !dma_fence_get_rcu(excl))
			continue;

		count = 0;
		reservation_object_for_each_shared(obj, iter) {
			if (dma_fence_is_signaled(iter.fence))
				continue;

			if (!dma_fence_get_rcu(iter.fence))
				break;

			if (count == sz) {
				struct dma_fence **nshared;

				sz = sz ? 2 * sz : 4;
				nshared = krealloc(shared,
						   sz * sizeof(*shared),
						   GFP_NOWAIT | __GFP_NOWARN);
				if (!nshared) {
					rcu_read_unlock();

					dma_fence_put(excl);
					dma_fence_put(iter.fence);
					while (count--)
						dma_fence_put(shared[count]);
					kfree(shared);

					shared = kmalloc(sz, GFP_TEMPORARY);
					if (!nshared)
						return -ENOMEM;

					rcu_read_lock();
					goto restart;
				}

				shared = nshared;
			}

			shared[count++] = iter.fence;
		}

		if (!read_seqcount_retry(&obj->seq, seq))
			break;

		while (count--)
			dma_fence_put(shared[count]);
		dma_fence_put(excl);
	}
	rcu_read_unlock();

	if (!count) {
		kfree(shared);
		shared = NULL;
	}

	*pshared_count = count;
	*pshared = shared;
	*pfence_excl = excl;
	return 0;
}
EXPORT_SYMBOL_GPL(reservation_object_get_fences_rcu);

/**
 * reservation_object_wait_timeout_rcu - Wait on reservation's objects
 * shared and/or exclusive fences.
 * @obj: the reservation object
 * @wait_all: if true, wait on all fences, else wait on just exclusive fence
 * @intr: if true, do interruptible wait
 * @timeout: timeout value in jiffies or zero to return immediately
 *
 * RETURNS
 * Returns -ERESTARTSYS if interrupted, 0 if the wait timed out, or
 * greater than zer on success.
 */
long reservation_object_wait_timeout_rcu(struct reservation_object *obj,
					 bool wait_all, bool intr,
					 long timeout)
{
	struct reservation_shared_iter iter;
	unsigned int seq;

	rcu_read_lock();
retry:
	seq = read_seqcount_begin(&obj->seq);

	if (wait_all) {
		reservation_object_for_each_shared(obj, iter)
			if (!dma_fence_is_signaled(iter.fence))
				goto wait;
	}

	iter.fence = rcu_dereference(obj->excl);
	if (iter.fence && !dma_fence_is_signaled(iter.fence))
		goto wait;

	if (read_seqcount_retry(&obj->seq, seq))
		goto retry;

	rcu_read_unlock();
	return timeout;

wait:
	if (!dma_fence_get_rcu(iter.fence))
		goto retry;
	rcu_read_unlock();

	timeout = dma_fence_wait_timeout(iter.fence, intr, timeout);
	dma_fence_put(iter.fence);
	if (timeout < 0)
		return timeout;

	rcu_read_lock();
	goto retry;
}
EXPORT_SYMBOL_GPL(reservation_object_wait_timeout_rcu);

/**
 * reservation_object_test_signaled_rcu - Test if a reservation object's
 * fences have been signaled.
 * @obj: the reservation object
 * @test_all: if true, test all fences, otherwise only test the exclusive
 * fence
 *
 * RETURNS
 * true if all fences signaled, else false
 */
bool reservation_object_test_signaled_rcu(struct reservation_object *obj,
					  bool test_all)
{
	struct reservation_shared_iter iter;
	bool ret = false;
	unsigned int seq;

	rcu_read_lock();
retry:
	seq = read_seqcount_begin(&obj->seq);

	if (test_all) {
		reservation_object_for_each_shared(obj, iter)
			if (!dma_fence_is_signaled(iter.fence))
				goto busy;
	}

	iter.fence = rcu_dereference(obj->excl);
	if (iter.fence && !dma_fence_is_signaled(iter.fence))
		goto busy;

	if (read_seqcount_retry(&obj->seq, seq))
		goto retry;

	ret = true;
busy:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(reservation_object_test_signaled_rcu);
