/*
 *	Handle bridge arp/nd proxy/suppress
 *
 *	Authors:
 *	Roopa Prabhu		<roopa@cumulusnetworks.com>
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/neighbour.h>
#include <net/arp.h>
#include <linux/if_vlan.h>
#include <linux/inetdevice.h>
#include <net/addrconf.h>
#include "br_private.h"

void br_recalculate_neigh_suppress_enabled(struct net_bridge *br)
{
	struct net_bridge_port *p;

	br->neigh_suppress_enabled = false;
	list_for_each_entry(p, &br->port_list, list) {
		if (p->flags & BR_NEIGH_SUPPRESS) {
			br->neigh_suppress_enabled = true;
			break;
		}
	}
}

static void br_arp_send(struct net_bridge_port *p, int type, int ptype,
			__be32 dest_ip, struct net_device *dev,
			__be32 src_ip, const unsigned char *dest_hw,
			const unsigned char *src_hw,
			const unsigned char *target_hw,
			__be16 vlan_proto, u16 vlan_tci)
{
	struct sk_buff *skb;

	netdev_dbg(dev, "arp send dev %s dst %pI4 dst_hw %pM src %pI4 src_hw %pM\n",
		   dev->name, &dest_ip, dest_hw, &src_ip, src_hw);

	if (!vlan_tci) {
		arp_send(type, ptype, dest_ip, dev, src_ip,
			 dest_hw, src_hw, target_hw);
		return;
	}

	skb = arp_create(type, ptype, dest_ip, dev, src_ip,
			 dest_hw, src_hw, target_hw);
	if (!skb)
		return;

	if (p) {
		struct net_bridge_vlan_group *vg;
		u16 pvid;

		vg = nbp_vlan_group_rcu(p);
		pvid = br_get_pvid(vg);
		if (pvid && pvid == vlan_tci)
			vlan_tci = 0;
	}

	if (vlan_tci) {
		skb = vlan_insert_tag_set_proto(skb, vlan_proto,
						vlan_tci);
		if (!skb) {
			net_err_ratelimited("%s: failed to insert VLAN tag\n",
					    __func__);
			return;
		}
	}

	arp_xmit(skb);
}

static int br_chk_addr_ip(struct net_device *dev, void *data)
{
	__be32 ip = *(__be32 *)data;
	struct in_device *in_dev;
	__be32 addr = 0;

	in_dev = __in_dev_get_rcu(dev);
	if (in_dev)
		addr = inet_confirm_addr(dev_net(dev), in_dev, 0, ip,
					 RT_SCOPE_HOST);

	if (addr == ip)
		return 1;

	return 0;
}

static bool br_is_local_ip(struct net_device *dev, __be32 ip)
{
	if (br_chk_addr_ip(dev, &ip))
		return true;

	/* check if ip is configured on upper dev */
	if (netdev_walk_all_upper_dev_rcu(dev, br_chk_addr_ip, &ip))
		return true;

	return false;
}

void br_do_proxy_suppress_arp(struct sk_buff *skb, struct net_bridge *br,
			      u16 vid, struct net_bridge_port *p)
{
	struct net_device *dev = br->dev;
	struct net_device *vlandev = NULL;
	struct neighbour *n;
	struct arphdr *parp;
	u8 *arpptr, *sha;
	__be32 sip, tip;

	BR_INPUT_SKB_CB(skb)->proxyarp_replied = false;

	if ((dev->flags & IFF_NOARP) ||
	    !pskb_may_pull(skb, arp_hdr_len(dev)))
		return;

	parp = arp_hdr(skb);

	if (parp->ar_pro != htons(ETH_P_IP) ||
	    parp->ar_hln != dev->addr_len ||
	    parp->ar_pln != 4)
		return;

	arpptr = (u8 *)parp + sizeof(struct arphdr);
	sha = arpptr;
	arpptr += dev->addr_len;	/* sha */
	memcpy(&sip, arpptr, sizeof(sip));
	arpptr += sizeof(sip);
	arpptr += dev->addr_len;	/* tha */
	memcpy(&tip, arpptr, sizeof(tip));

	if (ipv4_is_loopback(tip) ||
	    ipv4_is_multicast(tip))
		return;

	if (br->neigh_suppress_enabled) {
		if (p && (p->flags & BR_NEIGH_SUPPRESS))
			return;
		if (ipv4_is_zeronet(sip) || sip == tip) {
			/* prevent flooding to neigh suppress ports */
			BR_INPUT_SKB_CB(skb)->proxyarp_replied = true;
			return;
		}
	}

	if (parp->ar_op != htons(ARPOP_REQUEST))
		return;

	if (vid != 0) {
		vlandev = __vlan_find_dev_deep_rcu(br->dev, skb->vlan_proto,
						   vid);
		if (!vlandev)
			return;
	} else {
		vlandev = dev;
	}

	if (br->neigh_suppress_enabled && br_is_local_ip(vlandev, tip)) {
		/* its our local ip, so don't proxy reply
		 * and don't forward to neigh suppress ports
		 */
		BR_INPUT_SKB_CB(skb)->proxyarp_replied = true;
		return;
	}

	n = neigh_lookup(&arp_tbl, &tip, vlandev);
	if (n) {
		struct net_bridge_fdb_entry *f;

		if (!(n->nud_state & NUD_VALID)) {
			neigh_release(n);
			return;
		}

		f = br_fdb_find_rcu(br, n->ha, vid);
		if (f) {
			bool replied = false;

			if (f->dst && ((p->flags & BR_PROXYARP) ||
				       (f->dst->flags & BR_PROXYARP_WIFI) ||
				       (f->dst->flags & BR_NEIGH_SUPPRESS))) {
				if (!vid)
					br_arp_send(p, ARPOP_REPLY, ETH_P_ARP,
						    sip, skb->dev, tip, sha,
						    n->ha, sha, 0, 0);
				else
					br_arp_send(p, ARPOP_REPLY, ETH_P_ARP,
						    sip, skb->dev, tip, sha,
						    n->ha, sha, skb->vlan_proto,
						    skb_vlan_tag_get(skb));
				replied = true;
			}

			/* If we have replied or as long as we know the
			 * mac, indicate to arp replied
			 */
			if (replied || br->neigh_suppress_enabled)
				BR_INPUT_SKB_CB(skb)->proxyarp_replied = true;
		}

		neigh_release(n);
	}
}
