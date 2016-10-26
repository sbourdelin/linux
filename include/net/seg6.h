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

struct seg6_pernet_data {
	struct mutex lock;
	struct in6_addr __rcu *tun_src;
};

static inline struct seg6_pernet_data *seg6_pernet(struct net *net)
{
	return net->ipv6.seg6_data;
}

#endif
