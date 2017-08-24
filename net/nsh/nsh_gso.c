/*
 *	NSH GSO Support
 *
 *	Authors: Yi Yang (yi.y.yang@intel.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 *	Based on: net/mpls/mpls_gso.c
 */

#include <linux/err.h>
#include <linux/netdev_features.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <net/protocol.h>
#include <net/nsh.h>

static struct sk_buff *nsh_gso_segment(struct sk_buff *skb,
				       netdev_features_t features)
{
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	__be16 protocol = skb->protocol;
	__be16 inner_proto;
	u16 mac_offset = skb->mac_header;
	u16 mac_len = skb->mac_len;
	struct nsh_hdr *nsh;
	unsigned int nsh_hlen;
	const struct net_offload *ops;
	struct sk_buff *(*gso_inner_segment)(struct sk_buff *skb,
					     netdev_features_t features);

	skb_reset_network_header(skb);
	nsh = (struct nsh_hdr *)skb_network_header(skb);
	nsh_hlen = nsh_hdr_len(nsh);
	if (unlikely(!pskb_may_pull(skb, nsh_hlen)))
		goto out;

	__skb_pull(skb, nsh_hlen);

	skb_reset_mac_header(skb);
	skb_reset_mac_len(skb);

	rcu_read_lock();
	switch (nsh->next_proto) {
	case NSH_P_ETHERNET:
		inner_proto = htons(ETH_P_TEB);
		gso_inner_segment = skb_mac_gso_segment;
		break;
	case NSH_P_IPV4:
		inner_proto = htons(ETH_P_IP);
		ops = rcu_dereference(inet_offloads[inner_proto]);
		if (!ops || !ops->callbacks.gso_segment)
			goto out;
		gso_inner_segment = ops->callbacks.gso_segment;
		break;
	case NSH_P_IPV6:
		inner_proto = htons(ETH_P_IPV6);
		ops = rcu_dereference(inet6_offloads[inner_proto]);
		if (!ops || !ops->callbacks.gso_segment)
			goto out;
		gso_inner_segment = ops->callbacks.gso_segment;
		break;
	case NSH_P_NSH:
		inner_proto = htons(ETH_P_NSH);
		gso_inner_segment = nsh_gso_segment;
		break;
	default:
		skb_gso_error_unwind(skb, protocol, nsh_hlen, mac_offset,
				     mac_len);
		goto out;
	}

	skb->protocol = inner_proto;
	segs = gso_inner_segment(skb, features);
	if (IS_ERR_OR_NULL(segs)) {
		skb_gso_error_unwind(skb, protocol, nsh_hlen, mac_offset,
				     mac_len);
		goto out;
	}

	do {
		skb->mac_len = mac_len;
		skb->protocol = protocol;

		skb_reset_inner_network_header(skb);

		__skb_push(skb, nsh_hlen);

		skb_reset_mac_header(skb);
		skb_set_network_header(skb, mac_len);
	} while ((skb = skb->next));

out:
	rcu_read_unlock();
	return segs;
}

static struct packet_offload nsh_offload __read_mostly = {
	.type = cpu_to_be16(ETH_P_NSH),
	.priority = 15,
	.callbacks = {
		.gso_segment    =	nsh_gso_segment,
	},
};

static int __init nsh_gso_init(void)
{
	dev_add_offload(&nsh_offload);

	return 0;
}

fs_initcall(nsh_gso_init);
