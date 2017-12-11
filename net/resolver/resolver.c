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
#include <net/genetlink.h>
#include <net/ip.h>
#include <net/ip6_fib.h>
#include <net/lwtunnel.h>
#include <net/protocol.h>
#include <net/resolver.h>
#include <uapi/linux/genetlink.h>

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
				 size_t max_size, net_rslv_cmpfn cmp_fn,
				 const struct net_rslv_netlink_map *nlmap)
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
	nrslv->nlmap = nlmap;
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

/* Netlink access utility functions and structures. */

struct net_rslv_params {
	unsigned int timeout;
	__u8 key[MAX_ADDR_LEN];
	size_t keysize;
};

static int parse_nl_config(struct net_rslv *nrslv, struct genl_info *info,
			   struct net_rslv_params *np)
{
	if (!info->attrs[nrslv->nlmap->dst_attr] ||
	    nla_len(info->attrs[nrslv->nlmap->dst_attr]) !=
					nrslv->params.key_len)
		return -EINVAL;

	memset(np, 0, sizeof(*np));

	memcpy(np->key, nla_data(info->attrs[nrslv->nlmap->dst_attr]),
	       nla_len(info->attrs[nrslv->nlmap->dst_attr]));

	if (info->attrs[nrslv->nlmap->timo_attr])
		np->timeout = nla_get_u32(info->attrs[nrslv->nlmap->timo_attr]);

	return 0;
}

int net_rslv_nl_cmd_add(struct net_rslv *nrslv, struct sk_buff *skb,
			struct genl_info *info)
{
	struct net_rslv_params p;
	int err;

	err = parse_nl_config(nrslv, info, &p);
	if (err)
		return err;

	return net_rslv_lookup_and_create(nrslv, p.key, p.timeout);
}
EXPORT_SYMBOL_GPL(net_rslv_nl_cmd_add);

int net_rslv_nl_cmd_del(struct net_rslv *nrslv, struct sk_buff *skb,
			struct genl_info *info)
{
	struct net_rslv_params p;
	int err;

	err = parse_nl_config(nrslv, info, &p);
	if (err)
		return err;

	/* Treat removal as being resolved */
	net_rslv_resolved(nrslv, p.key);

	return 0;
}
EXPORT_SYMBOL_GPL(net_rslv_nl_cmd_del);

static int net_rslv_fill_info(struct net_rslv *nrslv,
			      struct net_rslv_ent *nrent,
			      struct sk_buff *msg)
{
	int from_now = 0;

	if (delayed_work_pending(&nrent->timeout_work)) {
		unsigned long expires = nrent->timeout_work.timer.expires;

		from_now = jiffies_to_msecs(expires - jiffies);

		if (from_now < 0)
			from_now = 0;
	}

	if (nla_put(msg, nrslv->nlmap->dst_attr, nrslv->params.key_len,
		    nrent->object) ||
	    nla_put_s32(msg, nrslv->nlmap->timo_attr, from_now))
		return -1;

	return 0;
}

static int net_rslv_dump_info(struct net_rslv *nrslv,
			      struct net_rslv_ent *nrent, u32 portid, u32 seq,
			      u32 flags, struct sk_buff *skb, u8 cmd)
{
	void *hdr;

	hdr = genlmsg_put(skb, portid, seq, nrslv->nlmap->genl_family, flags,
			  cmd);
	if (!hdr)
		return -ENOMEM;

	if (net_rslv_fill_info(nrslv, nrent, skb) < 0)
		goto nla_put_failure;

	genlmsg_end(skb, hdr);
	return 0;

nla_put_failure:
	genlmsg_cancel(skb, hdr);
	return -EMSGSIZE;
}

int net_rslv_nl_cmd_get(struct net_rslv *nrslv, struct sk_buff *skb,
			struct genl_info *info)
{
	struct net_rslv_ent *nrent;
	struct net_rslv_params p;
	struct sk_buff *msg;
	int err;

	err = parse_nl_config(nrslv, info, &p);
	if (err)
		return err;

	msg = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!msg)
		return -ENOMEM;

	rcu_read_lock();

	nrent = rhashtable_lookup_fast(&nrslv->rhash_table, p.key,
				       nrslv->params);
	if (nrent)
		err = net_rslv_dump_info(nrslv, nrent, info->snd_portid,
					 info->snd_seq, 0, msg,
					 info->genlhdr->cmd);

	rcu_read_unlock();

	if (err < 0)
		goto out_free;

	return genlmsg_reply(msg, info);

out_free:
	nlmsg_free(msg);
	return err;
}
EXPORT_SYMBOL_GPL(net_rslv_nl_cmd_get);

int net_rslv_nl_cmd_flush(struct net_rslv *nrslv, struct sk_buff *skb,
			  struct genl_info *info)
{
	struct rhashtable_iter iter;
	struct net_rslv_ent *nrent;
	spinlock_t *lock;
	int ret;

	ret = rhashtable_walk_init(&nrslv->rhash_table, &iter, GFP_KERNEL);
	if (ret)
		return ret;

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

		lock = net_rslv_get_lock(nrslv, nrent->object);

		spin_lock(lock);
		ret = rhashtable_remove_fast(&nrslv->rhash_table, &nrent->node,
					     nrslv->params);
		spin_unlock(lock);

		if (ret)
			break;
	}

	rhashtable_walk_stop(&iter);
	rhashtable_walk_exit(&iter);

	return ret;
}
EXPORT_SYMBOL_GPL(net_rslv_nl_cmd_flush);

int net_rslv_nl_dump_start(struct net_rslv *nrslv, struct netlink_callback *cb)
{
	struct rhashtable_iter *iter;
	int ret;

	iter = kmalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		return -ENOMEM;

	ret = rhashtable_walk_init(&nrslv->rhash_table, iter, GFP_KERNEL);
	if (ret) {
		kfree(iter);
		return ret;
	}

	cb->args[0] = (long)iter;

	return 0;
}
EXPORT_SYMBOL_GPL(net_rslv_nl_dump_start);

int net_rslv_nl_dump_done(struct net_rslv *nrslv, struct netlink_callback *cb)
{
	struct rhashtable_iter *iter =
				(struct rhashtable_iter *)cb->args[0];

	rhashtable_walk_exit(iter);

	kfree(iter);

	return 0;
}
EXPORT_SYMBOL_GPL(net_rslv_nl_dump_done);

int net_rslv_nl_dump(struct net_rslv *nrslv, struct sk_buff *skb,
		     struct netlink_callback *cb)
{
	struct rhashtable_iter *iter =
				(struct rhashtable_iter *)cb->args[0];
	struct net_rslv_ent *nrent;
	int ret;

	ret = rhashtable_walk_start_check(iter);
	if (ret)
		goto done;

	/* Get first entty */
	nrent = rhashtable_walk_peek(iter);

	for (;;) {
		if (IS_ERR(nrent)) {
			ret = PTR_ERR(nrent);
			if (ret == -EAGAIN) {
				/* Table has changed and iter has reset. Return
				 * -EAGAIN to the application even if we have
				 * written data to the skb. The application
				 * needs to deal with this.
				 */

				goto done;
			}
			break;
		} else if (!nrent) {
			break;
		}

		ret =  net_rslv_dump_info(nrslv, nrent,
					  NETLINK_CB(cb->skb).portid,
					  cb->nlh->nlmsg_seq,
					  NLM_F_MULTI, skb,
					  nrslv->nlmap->get_cmd);
		if (ret)
			break;

		/* Get next and advance the iter */
		nrent = rhashtable_walk_next(iter);
	}

	ret = (skb->len ? : ret);

done:
	rhashtable_walk_stop(iter);
	return ret;
}
EXPORT_SYMBOL_GPL(net_rslv_nl_dump);

MODULE_AUTHOR("Tom Herbert <tom@quantonium.net>");
MODULE_LICENSE("GPL");

