/*
 * See Documentation/skb-array.txt for more information.
 */

#ifndef _LINUX_SKB_ARRAY_H
#define _LINUX_SKB_ARRAY_H 1

#include <linux/spinlock.h>
#include <linux/cache.h>
#include <linux/types.h>
#include <linux/compiler.h>
#include <linux/cache.h>
#include <linux/slab.h>
#include <asm/errno.h>

struct sk_buff;

struct skb_array {
	int producer ____cacheline_aligned_in_smp;
	spinlock_t producer_lock;
	int consumer ____cacheline_aligned_in_smp;
	spinlock_t consumer_lock;
	/* Shared consumer/producer data */
	int size ____cacheline_aligned_in_smp; /* max entries in queue */
	struct sk_buff **queue;
};

/*
 * Fill several (currently 2) cache lines before producer tries to wrap around,
 * to avoid sharing a cache line between producer and consumer.  The bigger the
 * value, the less chance of a contention but the more cache pressure we put on
 * other users.  Change SKB_ARRAY_MIN_SIZE to INT_MAX to disable the heuristic
 * and wrap around only when we reach end of queue.
 */
#define SKB_ARRAY_MIN_SIZE (2 * cache_line_size() / sizeof (struct sk_buff *))

/* Note: callers invoking this in a loop must use a compiler barrier,
 * for example cpu_relax().
 */
static inline int __skb_array_produce(struct skb_array *a,
				       struct sk_buff *skb)
{
	/* Try to start from beginning: good for cache utilization as we'll
	 * keep reusing the same cache line.
	 * Produce at least SKB_ARRAY_MIN_SIZE entries before trying to do this,
	 * to reduce bouncing cache lines between them.
	 */
	if (a->producer >= SKB_ARRAY_MIN_SIZE && !a->queue[0])
		a->producer = 0;
	if (a->queue[a->producer])
		return -ENOSPC;
	a->queue[a->producer] = skb;
	if (unlikely(++a->producer > a->size))
		a->producer = 0;
	return 0;
}

static inline int skb_array_produce_bh(struct skb_array *a,
				       struct sk_buff *skb)
{
	int ret;

	spin_lock_bh(&a->producer_lock);
	ret = __skb_array_produce(a, skb);
	spin_unlock_bh(&a->producer_lock);

	return ret;
}

/* Note: callers invoking this in a loop must use a compiler barrier,
 * for example cpu_relax().
 */
static inline struct sk_buff *__skb_array_peek(struct skb_array *a)
{
	if (a->queue[a->consumer])
		return a->queue[a->consumer];

	/* Check whether producer started at the beginning. */
	if (unlikely(a->consumer >= SKB_ARRAY_MIN_SIZE && a->queue[0])) {
		a->consumer = 0;
		return a->queue[0];
	}

	return NULL;
}

static inline void __skb_array_consume(struct skb_array *a)
{
	a->queue[a->consumer++] = NULL;
	if (unlikely(a->consumer > a->size))
		a->consumer = 0;
}

static inline struct sk_buff *skb_array_consume_bh(struct skb_array *a)
{
	struct sk_buff *skb;

	spin_lock_bh(&a->producer_lock);
	skb = __skb_array_peek(a);
	if (skb)
		__skb_array_consume(a);
	spin_unlock_bh(&a->producer_lock);

	return skb;
}

static inline int skb_array_init(struct skb_array *a, int size, gfp_t gfp)
{
	a->queue = kmalloc(ALIGN(size * sizeof *(a->queue), SMP_CACHE_BYTES),
			   gfp);
	if (!a->queue)
		return -ENOMEM;

	a->size = size;
	a->producer = a->consumer = 0;
	spin_lock_init(&a->producer_lock);
	spin_lock_init(&a->consumer_lock);

	return 0;
}

static inline void skb_array_cleanup(struct skb_array *a)
{
	kfree(a->queue);
}

#endif /* _LINUX_SKB_ARRAY_H  */
