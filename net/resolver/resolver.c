/*
 * net/core/resolver.c - Generic network address resolver backend
 *
 * Copyright (c) 2017 Tom Herbert <tom@quantonium.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/errno.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <net/checksum.h>
#include <net/ip.h>
#include <net/ip6_fib.h>
#include <net/lwtunnel.h>
#include <net/protocol.h>
#include <net/resolver.h>

struct net_rslv_ent {
	struct rhash_head node;
	struct delayed_work timeout_work;
	struct net_rslv *nrslv;

	struct rcu_head rcu;

	char object[];
};

static void net_rslv_destroy_rcu(struct rcu_head *head)
{
	struct net_rslv_ent *nrent = container_of(head, struct net_rslv_ent,
						  rcu);
	kfree(nrent);
}

static void net_rslv_destroy_entry(struct net_rslv *nrslv,
				   struct net_rslv_ent *nrent)
{
	call_rcu(&nrent->rcu, net_rslv_destroy_rcu);
}

static inline spinlock_t *net_rslv_get_lock(struct net_rslv *nrslv, void *key)
{
	unsigned int hash;

	/* Use the rhashtable hash function */
	hash = rht_key_get_hash(&nrslv->rhash_table, key, nrslv->params,
				nrslv->hash_rnd);

	return &nrslv->locks[hash & nrslv->locks_mask];
}

static void net_rslv_delayed_work(struct work_struct *w)
{
	struct delayed_work *delayed_work = to_delayed_work(w);
	struct net_rslv_ent *nrent = container_of(delayed_work,
						  struct net_rslv_ent,
						  timeout_work);
	struct net_rslv *nrslv = nrent->nrslv;
	spinlock_t *lock = net_rslv_get_lock(nrslv, nrent->object);

	spin_lock(lock);
	rhashtable_remove_fast(&nrslv->rhash_table, &nrent->node,
			       nrslv->params);
	spin_unlock(lock);

	net_rslv_destroy_entry(nrslv, nrent);
}

static void net_rslv_ent_free_cb(void *ptr, void *arg)
{
	struct net_rslv_ent *nrent = (struct net_rslv_ent *)ptr;
	struct net_rslv *nrslv = nrent->nrslv;

	net_rslv_destroy_entry(nrslv, nrent);
}

void net_rslv_resolved(struct net_rslv *nrslv, void *key)
{
	spinlock_t *lock = net_rslv_get_lock(nrslv, key);
	struct net_rslv_ent *nrent;

	rcu_read_lock();

	nrent = rhashtable_lookup_fast(&nrslv->rhash_table, key,
				       nrslv->params);
	if (!nrent)
		goto out;

	/* Cancel timer first */
	cancel_delayed_work_sync(&nrent->timeout_work);

	spin_lock(lock);

	/* Lookup again just in case someone already removed */
	nrent = rhashtable_lookup_fast(&nrslv->rhash_table, key,
				       nrslv->params);
	if (unlikely(!nrent)) {
		spin_unlock(lock);
		goto out;
	}

	rhashtable_remove_fast(&nrslv->rhash_table, &nrent->node,
			       nrslv->params);
	spin_unlock(lock);

	net_rslv_destroy_entry(nrslv, nrent);

out:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(net_rslv_resolved);

/* Called with hash bucket lock held */
static int net_rslv_new_ent(struct net_rslv *nrslv, void *key,
			    unsigned int timeout)
{
	struct net_rslv_ent *nrent;
	int err;

	nrent = kzalloc(sizeof(*nrent) + nrslv->obj_size, GFP_KERNEL);
	if (!nrent)
		return -ENOMEM;

	/* Key is always at beginning of object data */
	memcpy(nrent->object, key, nrslv->params.key_len);

	nrent->nrslv = nrslv;

	/* Put in hash table */
	err = rhashtable_lookup_insert_fast(&nrslv->rhash_table,
					    &nrent->node, nrslv->params);
	if (err) {
		kfree(nrent);
		return err;
	}

	if (timeout) {
		/* Schedule timeout for resolver */
		INIT_DELAYED_WORK(&nrent->timeout_work, net_rslv_delayed_work);
		schedule_delayed_work(&nrent->timeout_work,
				      msecs_to_jiffies(timeout));
	}

	return 0;
}

int net_rslv_lookup_and_create(struct net_rslv *nrslv, void *key,
			       unsigned int timeout)
{
	spinlock_t *lock = net_rslv_get_lock(nrslv, key);
	int ret;

	if (rhashtable_lookup_fast(&nrslv->rhash_table, key, nrslv->params))
		return -EEXIST;

	spin_lock(lock);

	/* Check if someone beat us to the punch */
	if (rhashtable_lookup_fast(&nrslv->rhash_table, key, nrslv->params)) {
		spin_unlock(lock);
		return -EEXIST;
	}

	ret = net_rslv_new_ent(nrslv, key, timeout);

	spin_unlock(lock);

	return ret;
}
EXPORT_SYMBOL_GPL(net_rslv_lookup_and_create);

static int net_rslv_cmp(struct rhashtable_compare_arg *arg,
			const void *obj)
{
	struct net_rslv *nrslv = container_of(arg->ht, struct net_rslv,
					      rhash_table);

	return nrslv->rslv_cmp(nrslv, arg->key, obj);
}

#define LOCKS_PER_CPU	10
#define MAX_LOCKS 1024

struct net_rslv *net_rslv_create(size_t obj_size, size_t key_len,
				 size_t max_size,
				 net_rslv_cmpfn cmp_fn)
{
	struct net_rslv *nrslv;
	int err;

	if (key_len < obj_size)
		return ERR_PTR(-EINVAL);

	nrslv = kzalloc(sizeof(*nrslv), GFP_KERNEL);
	if (!nrslv)
		return ERR_PTR(-ENOMEM);

	err = alloc_bucket_spinlocks(&nrslv->locks, &nrslv->locks_mask,
				     MAX_LOCKS, LOCKS_PER_CPU, GFP_KERNEL);
	if (err)
		return ERR_PTR(err);

	nrslv->obj_size = obj_size;
	nrslv->rslv_cmp = cmp_fn;
	get_random_bytes(&nrslv->hash_rnd, sizeof(nrslv->hash_rnd));

	nrslv->params.head_offset = offsetof(struct net_rslv_ent, node);
	nrslv->params.key_offset = offsetof(struct net_rslv_ent, object);
	nrslv->params.key_len = key_len;
	nrslv->params.max_size = max_size;
	nrslv->params.min_size = 256;
	nrslv->params.automatic_shrinking = true;
	nrslv->params.obj_cmpfn = cmp_fn ? net_rslv_cmp : NULL;

	rhashtable_init(&nrslv->rhash_table, &nrslv->params);

	return nrslv;
}
EXPORT_SYMBOL_GPL(net_rslv_create);

static void net_rslv_cancel_all_delayed_work(struct net_rslv *nrslv)
{
	struct rhashtable_iter iter;
	struct net_rslv_ent *nrent;
	int ret;

	ret = rhashtable_walk_init(&nrslv->rhash_table, &iter, GFP_ATOMIC);
	if (WARN_ON(ret))
		return;

	rhashtable_walk_start(&iter);

	for (;;) {
		nrent = rhashtable_walk_next(&iter);

		if (IS_ERR(nrent)) {
			if (PTR_ERR(nrent) == -EAGAIN) {
				/* New table, we're okay to continue */
				continue;
			}
			ret = PTR_ERR(nrent);
			break;
		} else if (!nrent) {
			break;
		}

		cancel_delayed_work_sync(&nrent->timeout_work);
	}

	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);
}

void net_rslv_destroy(struct net_rslv *nrslv)
{
	/* First cancel delayed work in all the nodes. We don't want
	 * delayed work trying to remove nodes from the table while
	 * rhashtable_free_and_destroy is walking.
	 */
	net_rslv_cancel_all_delayed_work(nrslv);

	rhashtable_free_and_destroy(&nrslv->rhash_table,
				    net_rslv_ent_free_cb, NULL);

	free_bucket_spinlocks(nrslv->locks);

	kfree(nrslv);
}
EXPORT_SYMBOL_GPL(net_rslv_destroy);

MODULE_AUTHOR("Tom Herbert <tom@quantonium.net>");
MODULE_LICENSE("GPL");

