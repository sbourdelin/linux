/*
 * Generic network address resovler backend
 *
 * Copyright (c) 2017 Tom Herbert <tom@quantonium.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef __NET_RESOLVER_H
#define __NET_RESOLVER_H

#include <linux/rhashtable.h>
#include <linux/types.h>
#include <net/genetlink.h>
#include <uapi/linux/genetlink.h>

struct net_rslv;

typedef int (*net_rslv_cmpfn)(struct net_rslv *nrslv, const void *key,
			      const void *object);

struct net_rslv_netlink_map {
	int dst_attr;
	int timo_attr;
	int get_cmd;
	struct genl_family *genl_family;
};

struct net_rslv {
	struct rhashtable rhash_table;
	struct rhashtable_params params;
	net_rslv_cmpfn rslv_cmp;
	size_t obj_size;
	spinlock_t *locks;
	unsigned int locks_mask;
	unsigned int hash_rnd;
	const struct net_rslv_netlink_map *nlmap;
};

struct net_rslv *net_rslv_create(size_t obj_size, size_t key_len,
				 size_t max_size, net_rslv_cmpfn cmp_fn,
				 const struct net_rslv_netlink_map *nlmap);

void net_rslv_destroy(struct net_rslv *nrslv);

int net_rslv_lookup_and_create(struct net_rslv *nrslv, void *key,
			       unsigned int timeout);

void net_rslv_resolved(struct net_rslv *nrslv, void *key);

int net_rslv_nl_cmd_add(struct net_rslv *nrslv, struct sk_buff *skb,
			struct genl_info *info);
int net_rslv_nl_cmd_del(struct net_rslv *nrslv, struct sk_buff *skb,
			struct genl_info *info);
int net_rslv_nl_cmd_get(struct net_rslv *nrslv, struct sk_buff *skb,
			struct genl_info *info);
int net_rslv_nl_cmd_flush(struct net_rslv *nrslv, struct sk_buff *skb,
			  struct genl_info *info);
int net_rslv_nl_dump_start(struct net_rslv *nrslv, struct netlink_callback *cb);
int net_rslv_nl_dump_done(struct net_rslv *nrslv, struct netlink_callback *cb);
int net_rslv_nl_dump(struct net_rslv *nrslv, struct sk_buff *skb,
		     struct netlink_callback *cb);

#endif /* __NET_RESOLVER_H */
