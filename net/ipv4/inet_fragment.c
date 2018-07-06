/*
 * inet fragments management
 *
 *		This program is free software; you can redistribute it and/or
 *		modify it under the terms of the GNU General Public License
 *		as published by the Free Software Foundation; either version
 *		2 of the License, or (at your option) any later version.
 *
 * 		Authors:	Pavel Emelyanov <xemul@openvz.org>
 *				Started as consolidation of ipv4/ip_fragment.c,
 *				ipv6/reassembly. and ipv6 nf conntrack reassembly
 */

#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/random.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/rhashtable.h>

#include <net/sock.h>
#include <net/inet_frag.h>
#include <net/inet_ecn.h>

/* Given the OR values of all fragments, apply RFC 3168 5.3 requirements
 * Value : 0xff if frame should be dropped.
 *         0 or INET_ECN_CE value, to be ORed in to final iph->tos field
 */
const u8 ip_frag_ecn_table[16] = {
	/* at least one fragment had CE, and others ECT_0 or ECT_1 */
	[IPFRAG_ECN_CE | IPFRAG_ECN_ECT_0]			= INET_ECN_CE,
	[IPFRAG_ECN_CE | IPFRAG_ECN_ECT_1]			= INET_ECN_CE,
	[IPFRAG_ECN_CE | IPFRAG_ECN_ECT_0 | IPFRAG_ECN_ECT_1]	= INET_ECN_CE,

	/* invalid combinations : drop frame */
	[IPFRAG_ECN_NOT_ECT | IPFRAG_ECN_CE] = 0xff,
	[IPFRAG_ECN_NOT_ECT | IPFRAG_ECN_ECT_0] = 0xff,
	[IPFRAG_ECN_NOT_ECT | IPFRAG_ECN_ECT_1] = 0xff,
	[IPFRAG_ECN_NOT_ECT | IPFRAG_ECN_ECT_0 | IPFRAG_ECN_ECT_1] = 0xff,
	[IPFRAG_ECN_NOT_ECT | IPFRAG_ECN_CE | IPFRAG_ECN_ECT_0] = 0xff,
	[IPFRAG_ECN_NOT_ECT | IPFRAG_ECN_CE | IPFRAG_ECN_ECT_1] = 0xff,
	[IPFRAG_ECN_NOT_ECT | IPFRAG_ECN_CE | IPFRAG_ECN_ECT_0 | IPFRAG_ECN_ECT_1] = 0xff,
};
EXPORT_SYMBOL(ip_frag_ecn_table);

int inet_frags_init(struct inet_frags *f)
{
	f->frags_cachep = kmem_cache_create(f->frags_cache_name, f->qsize, 0, 0,
					    NULL);
	if (!f->frags_cachep)
		return -ENOMEM;

	return 0;
}
EXPORT_SYMBOL(inet_frags_init);

void inet_frags_fini(struct inet_frags *f)
{
	/* We must wait that all inet_frag_destroy_rcu() have completed. */
	rcu_barrier();

	kmem_cache_destroy(f->frags_cachep);
	f->frags_cachep = NULL;
}
EXPORT_SYMBOL(inet_frags_fini);

static void inet_frags_free_cb(void *ptr, void *arg)
{
	struct inet_frag_queue *fq = ptr;

	/* If we can not cancel the timer, it means this frag_queue
	 * is already disappearing, we have nothing to do.
	 * Otherwise, we own a refcount until the end of this function.
	 */
	if (!del_timer(&fq->timer))
		return;

	spin_lock_bh(&fq->lock);
	if (!(fq->flags & INET_FRAG_COMPLETE)) {
		fq->flags |= INET_FRAG_COMPLETE;
		refcount_dec(&fq->refcnt);
	}
	spin_unlock_bh(&fq->lock);

	inet_frag_put(fq);
}

static void inet_frag_schedule_worker(struct netns_frags *nf)
{
	if (unlikely(!work_pending(&nf->frags_work)))
		schedule_work(&nf->frags_work);
}

#define INETFRAGS_EVICT_MAX	64
static void inet_frag_worker(struct work_struct *work)
{
	struct netns_frags *nf;
	bool reschedule;
	int evicted = 0;

	nf = container_of(work, struct netns_frags, frags_work);

	rhashtable_walk_start(&nf->iter);

	while ((reschedule = (frag_mem_limit(nf) > nf->low_thresh))) {
		struct inet_frag_queue *fq = rhashtable_walk_next(&nf->iter);

		if (IS_ERR(fq) && PTR_ERR(fq) == -EAGAIN)
			continue;
		if (!fq) {
			/* end of table, restart the walk */
			rhashtable_walk_stop(&nf->iter);
			rhashtable_walk_exit(&nf->iter);
			rhashtable_walk_enter(&nf->rhashtable, &nf->iter);
			rhashtable_walk_start(&nf->iter);
			continue;
		}
		if (!refcount_inc_not_zero(&fq->refcnt))
			continue;

		spin_lock_bh(&fq->lock);
		inet_frag_kill(fq);
		spin_unlock_bh(&fq->lock);
		inet_frag_put(fq);

		/* limit the amount of work we can do before a reschedule,
		 * to avoid starving others queued works
		 */
		if (++evicted > INETFRAGS_EVICT_MAX)
			break;
	}

	rhashtable_walk_stop(&nf->iter);

	if (reschedule)
		inet_frag_schedule_worker(nf);
}

int inet_frags_init_net(struct netns_frags *nf)
{
	int ret;

	atomic_long_set(&nf->mem, 0);
	INIT_WORK(&nf->frags_work, inet_frag_worker);
	ret = rhashtable_init(&nf->rhashtable, &nf->f->rhash_params);
	if (ret)
		return ret;
	rhashtable_walk_enter(&nf->rhashtable, &nf->iter);
	return ret;
}
EXPORT_SYMBOL(inet_frags_init_net);

void inet_frags_exit_net(struct netns_frags *nf)
{
	nf->low_thresh = 0; /* prevent creation of new frags */
	cancel_work_sync(&nf->frags_work);
	rhashtable_walk_exit(&nf->iter);
	rhashtable_free_and_destroy(&nf->rhashtable, inet_frags_free_cb, NULL);
}
EXPORT_SYMBOL(inet_frags_exit_net);

void inet_frag_kill(struct inet_frag_queue *fq)
{
	if (del_timer(&fq->timer))
		refcount_dec(&fq->refcnt);

	if (!(fq->flags & INET_FRAG_COMPLETE)) {
		struct netns_frags *nf = fq->net;

		fq->flags |= INET_FRAG_COMPLETE;
		rhashtable_remove_fast(&nf->rhashtable, &fq->node, nf->f->rhash_params);
		refcount_dec(&fq->refcnt);
	}
}
EXPORT_SYMBOL(inet_frag_kill);

static void inet_frag_destroy_rcu(struct rcu_head *head)
{
	struct inet_frag_queue *q = container_of(head, struct inet_frag_queue,
						 rcu);
	struct inet_frags *f = q->net->f;

	if (f->destructor)
		f->destructor(q);
	kmem_cache_free(f->frags_cachep, q);
}

void inet_frag_destroy(struct inet_frag_queue *q)
{
	struct sk_buff *fp;
	struct netns_frags *nf;
	unsigned int sum, sum_truesize = 0;
	struct inet_frags *f;

	WARN_ON(!(q->flags & INET_FRAG_COMPLETE));
	WARN_ON(del_timer(&q->timer) != 0);

	/* Release all fragment data. */
	fp = q->fragments;
	nf = q->net;
	f = nf->f;
	while (fp) {
		struct sk_buff *xp = fp->next;

		sum_truesize += fp->truesize;
		kfree_skb(fp);
		fp = xp;
	}
	sum = sum_truesize + f->qsize;

	call_rcu(&q->rcu, inet_frag_destroy_rcu);

	sub_frag_mem_limit(nf, sum);
}
EXPORT_SYMBOL(inet_frag_destroy);

static struct inet_frag_queue *inet_frag_alloc(struct netns_frags *nf,
					       struct inet_frags *f,
					       void *arg)
{
	struct inet_frag_queue *q;

	if (!nf->high_thresh || frag_mem_limit(nf) > nf->high_thresh) {
		inet_frag_schedule_worker(nf);
		return NULL;
	}

	q = kmem_cache_zalloc(f->frags_cachep, GFP_ATOMIC);
	if (!q)
		return NULL;

	q->net = nf;
	f->constructor(q, arg);
	add_frag_mem_limit(nf, f->qsize);

	timer_setup(&q->timer, f->frag_expire, 0);
	spin_lock_init(&q->lock);
	refcount_set(&q->refcnt, 3);

	return q;
}

static struct inet_frag_queue *inet_frag_create(struct netns_frags *nf,
						void *arg)
{
	struct inet_frags *f = nf->f;
	struct inet_frag_queue *q;
	int err;

	q = inet_frag_alloc(nf, f, arg);
	if (!q)
		return NULL;

	mod_timer(&q->timer, jiffies + nf->timeout);

	err = rhashtable_insert_fast(&nf->rhashtable, &q->node,
				     f->rhash_params);
	if (err < 0) {
		q->flags |= INET_FRAG_COMPLETE;
		inet_frag_kill(q);
		inet_frag_destroy(q);
		return NULL;
	}
	return q;
}

/* TODO : call from rcu_read_lock() and no longer use refcount_inc_not_zero() */
struct inet_frag_queue *inet_frag_find(struct netns_frags *nf, void *key)
{
	struct inet_frag_queue *fq;

	rcu_read_lock();

	fq = rhashtable_lookup(&nf->rhashtable, key, nf->f->rhash_params);
	if (fq) {
		if (!refcount_inc_not_zero(&fq->refcnt))
			fq = NULL;
		rcu_read_unlock();
		return fq;
	}
	rcu_read_unlock();

	return inet_frag_create(nf, key);
}
EXPORT_SYMBOL(inet_frag_find);
