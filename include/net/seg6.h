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
#include <net/lwtunnel.h>
#include <net/seg6_hmac.h>

struct seg6_pernet_data {
	struct mutex lock;
	struct in6_addr __rcu *tun_src;
	struct list_head hmac_infos;
};

static inline struct seg6_pernet_data *seg6_pernet(struct net *net)
{
	return net->ipv6.seg6_data;
}

static inline void seg6_pernet_lock(struct net *net)
{
	mutex_lock(&seg6_pernet(net)->lock);
}

static inline void seg6_pernet_unlock(struct net *net)
{
	mutex_unlock(&seg6_pernet(net)->lock);
}

static inline struct seg6_iptunnel_encap *
seg6_lwtunnel_encap(struct lwtunnel_state *lwtstate)
{
	return (struct seg6_iptunnel_encap *)lwtstate->data;
}

extern struct sr6_tlv_hmac *seg6_get_tlv_hmac(struct ipv6_sr_hdr *srh);

#endif
