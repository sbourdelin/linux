/*
 *  SR-IPv6 implementation
 *
 *  Author:
 *  David Lebrun <david.lebrun@uclouvain.be>
 *
 *
 *  This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#ifndef _NET_SEG6_H
#define _NET_SEG6_H

#include <linux/net.h>
#include <linux/ipv6.h>
#if IS_ENABLED(CONFIG_IPV6_SEG6_IPTUNNEL)
#include <net/lwtunnel.h>
#endif

#define SEG6_VERSION_MAJOR	0
#define SEG6_VERSION_MINOR	30

struct seg6_pernet_data {
	spinlock_t lock;
	struct in6_addr __rcu *tun_src;
};

static inline struct seg6_pernet_data *seg6_pernet(struct net *net)
{
	return net->ipv6.seg6_data;
}

static inline void seg6_pernet_lock(struct net *net)
{
	spin_lock(&seg6_pernet(net)->lock);
}

static inline void seg6_pernet_unlock(struct net *net)
{
	spin_unlock(&seg6_pernet(net)->lock);
}

#if IS_ENABLED(CONFIG_IPV6_SEG6_IPTUNNEL)
static inline struct seg6_iptunnel_encap *
seg6_lwtunnel_encap(struct lwtunnel_state *lwtstate)
{
	return (struct seg6_iptunnel_encap *)lwtstate->data;
}
#endif

#endif
