/*
 * Header file for reservations for dma-buf and ttm
 *
 * Copyright(C) 2011 Linaro Limited. All rights reserved.
 * Copyright (C) 2012-2013 Canonical Ltd
 * Copyright (C) 2012 Texas Instruments
 *
 * Authors:
 * Rob Clark <robdclark@gmail.com>
 * Maarten Lankhorst <maarten.lankhorst@canonical.com>
 * Thomas Hellstrom <thellstrom-at-vmware-dot-com>
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
 */
#ifndef _LINUX_RESERVATION_H
#define _LINUX_RESERVATION_H

#include <linux/ww_mutex.h>
#include <linux/dma-fence.h>
#include <linux/slab.h>
#include <linux/seqlock.h>
#include <linux/rcupdate.h>

extern struct ww_class reservation_ww_class;
extern struct lock_class_key reservation_seqcount_class;
extern const char reservation_seqcount_string[];

struct reservation_shared_layer;

#define NSHARED 16

struct reservation_shared_layer {
	union {
		u64 prefix;
		struct rcu_head rcu;
	};
	unsigned int height;
	unsigned int bitmap;
	void *slot[NSHARED];
	struct reservation_shared_layer *parent;
};

struct reservation_shared {
	struct reservation_shared_layer *hint;
	struct reservation_shared_layer *top;
	struct reservation_shared_layer *freed;
};

static inline void reservation_shared_init(struct reservation_shared *shared)
{
	memset(shared, 0, sizeof(*shared));
}

void reservation_shared_destroy(struct reservation_shared *shared);

/**
 * struct reservation_object - a reservation object manages fences for a buffer
 * @lock: update side lock
 * @seq: sequence count for managing RCU read-side synchronization
 * @excl: the exclusive fence, if there is one currently
 * @shared: list of current shared fences
 */
struct reservation_object {
	struct ww_mutex lock;
	seqcount_t seq;

	struct dma_fence __rcu *excl;
	struct reservation_shared shared;
};

#define reservation_object_held(obj) lockdep_is_held(&(obj)->lock.base)
#define reservation_object_assert_held(obj) \
	lockdep_assert_held(&(obj)->lock.base)

/**
 * reservation_object_init - initialize a reservation object
 * @obj: the reservation object
 */
static inline void
reservation_object_init(struct reservation_object *obj)
{
	ww_mutex_init(&obj->lock, &reservation_ww_class);

	__seqcount_init(&obj->seq, reservation_seqcount_string, &reservation_seqcount_class);
	RCU_INIT_POINTER(obj->excl, NULL);
	reservation_shared_init(&obj->shared);
}

/**
 * reservation_object_fini - destroys a reservation object
 * @obj: the reservation object
 */
static inline void
reservation_object_fini(struct reservation_object *obj)
{
	/*
	 * This object should be dead and all references must have
	 * been released to it, so no need to be protected with rcu.
	 */
	dma_fence_put(rcu_dereference_protected(obj->excl, 1));

	reservation_shared_destroy(&obj->shared);

	ww_mutex_destroy(&obj->lock);
}

/**
 * reservation_object_get_excl - get the reservation object's
 * exclusive fence, with update-side lock held
 * @obj: the reservation object
 *
 * Returns the exclusive fence (if any).  Does NOT take a
 * reference.  The obj->lock must be held.
 *
 * RETURNS
 * The exclusive fence or NULL
 */
static inline struct dma_fence *
reservation_object_get_excl(struct reservation_object *obj)
{
	return rcu_dereference_protected(obj->excl,
					 reservation_object_held(obj));
}

/**
 * reservation_object_get_excl_rcu - get the reservation object's
 * exclusive fence, without lock held.
 * @obj: the reservation object
 *
 * If there is an exclusive fence, this atomically increments it's
 * reference count and returns it.
 *
 * RETURNS
 * The exclusive fence or NULL if none
 */
static inline struct dma_fence *
reservation_object_get_excl_rcu(struct reservation_object *obj)
{
	struct dma_fence *fence;
	unsigned seq;
retry:
	seq = read_seqcount_begin(&obj->seq);
	rcu_read_lock();
	fence = rcu_dereference(obj->excl);
	if (read_seqcount_retry(&obj->seq, seq)) {
		rcu_read_unlock();
		goto retry;
	}
	fence = dma_fence_get(fence);
	rcu_read_unlock();
	return fence;
}

static inline bool
reservation_object_has_shared(struct reservation_object *obj)
{
	return READ_ONCE(obj->shared.top);
}

int reservation_object_reserve_shared(struct reservation_object *obj);
void reservation_object_add_shared_fence(struct reservation_object *obj,
					 struct dma_fence *fence);

void reservation_object_add_excl_fence(struct reservation_object *obj,
				       struct dma_fence *fence);

int reservation_object_get_fences_rcu(struct reservation_object *obj,
				      struct dma_fence **pfence_excl,
				      unsigned *pshared_count,
				      struct dma_fence ***pshared);

long reservation_object_wait_timeout_rcu(struct reservation_object *obj,
					 bool wait_all, bool intr,
					 long timeout);

bool reservation_object_test_signaled_rcu(struct reservation_object *obj,
					  bool test_all);

struct reservation_shared_iter {
	struct dma_fence *fence;
	struct reservation_shared_layer *p;
	u8 stack[16];
};

static inline void
__reservation_shared_iter_fill(struct reservation_shared_iter *iter,
			       struct reservation_shared_layer *p)
{
	int h;

	do {
		int pos = ffs(p->bitmap) - 1;

		h = p->height / ilog2(NSHARED);
		iter->stack[h] = pos;

		iter->p = p;
		p = p->slot[pos];
	} while (h);

	iter->fence = (void *)p;
}

static inline void
reservation_shared_iter_init(struct reservation_object *obj,
			     struct reservation_shared_iter *iter)
{
	if (obj->shared.top)
		__reservation_shared_iter_fill(iter, obj->shared.top);
	else
		iter->fence = NULL;
}

#define fns(x, bit) (ffs((x) & (~(typeof(x))0 << (bit))))

void __reservation_shared_iter_next(struct reservation_shared_iter *iter);

static inline void
reservation_shared_iter_next(struct reservation_shared_iter *iter)
{
	int pos;

	pos = fns(iter->p->bitmap, iter->stack[0] + 1);
	if (likely(pos)) {
		iter->stack[0] = --pos;
		iter->fence = iter->p->slot[pos];
	} else {
		__reservation_shared_iter_next(iter);
	}
}

#define reservation_object_for_each_shared(obj, __i)			\
	for (reservation_shared_iter_init(obj, &(__i));			\
	     __i.fence;							\
	     reservation_shared_iter_next(&(__i)))

#endif /* _LINUX_RESERVATION_H */
