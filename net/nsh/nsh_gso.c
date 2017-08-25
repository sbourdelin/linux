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

struct sk_buff *nsh_gso_segment(struct sk_buff *skb,
				netdev_features_t features)
{
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	int nshoff;
	__be16 inner_proto;
	struct nsh_hdr *nsh;
	unsigned int nsh_hlen;
	struct packet_offload *po;
	struct sk_buff *(*gso_inner_segment)(struct sk_buff *skb,
					     netdev_features_t features);

	skb_reset_network_header(skb);
	nshoff = skb_network_header(skb) - skb_mac_header(skb);

	if (unlikely(!pskb_may_pull(skb, NSH_BASE_HDR_LEN)))
		goto out;

	nsh = (struct nsh_hdr *)skb_network_header(skb);
	nsh_hlen = nsh_hdr_len(nsh);
	if (unlikely(!pskb_may_pull(skb, nsh_hlen)))
		goto out;

	nsh = (struct nsh_hdr *)skb_network_header(skb);
	__skb_pull(skb, nsh_hlen);

	skb_reset_transport_header(skb);

	switch (nsh->np) {
	case NSH_P_ETHERNET:
		inner_proto = htons(ETH_P_TEB);
		gso_inner_segment = skb_mac_gso_segment;
		break;
	case NSH_P_IPV4:
		inner_proto = htons(ETH_P_IP);
		po = find_gso_segment_by_type(inner_proto);
		if (!po || !po->callbacks.gso_segment)
			goto out;
		gso_inner_segment = po->callbacks.gso_segment;
		break;
	case NSH_P_IPV6:
		inner_proto = htons(ETH_P_IPV6);
		po = find_gso_segment_by_type(inner_proto);
		if (!po || !po->callbacks.gso_segment)
			goto out;
		gso_inner_segment = po->callbacks.gso_segment;
		break;
	case NSH_P_NSH:
		inner_proto = htons(ETH_P_NSH);
		gso_inner_segment = nsh_gso_segment;
		break;
	default:
		goto out;
	}

	segs = gso_inner_segment(skb, features);
	if (IS_ERR_OR_NULL(segs))
		goto out;

	skb = segs;
	do {
		nsh = (struct nsh_hdr *)(skb_mac_header(skb) + nshoff);
		skb->network_header = (u8 *)nsh - skb->head;
	} while ((skb = skb->next));

out:
	return segs;
}
EXPORT_SYMBOL(nsh_gso_segment);

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

device_initcall(nsh_gso_init);
