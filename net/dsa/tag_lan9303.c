/*
 * Copyright (C) 2017 Pengutronix, Juergen Borleis <jbe@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/etherdevice.h>
#include <linux/list.h>
#include <linux/slab.h>
#include "dsa_priv.h"

/* To define the outgoing port and to discover the incoming port a regular
 * VLAN tag is used by the LAN9303. But its VID meaning is 'special':
 *
 *       Dest MAC       Src MAC        TAG    Type
 * ...| 1 2 3 4 5 6 | 1 2 3 4 5 6 | 1 2 3 4 | 1 2 |...
 *                                |<------->|
 * TAG:
 *    |<------------->|
 *    |  1  2 | 3  4  |
 *      TPID    VID
 *     0x8100
 *
 * VID bit 3 indicates a request for an ALR lookup.
 *
 * If VID bit 3 is zero, then bits 0 and 1 specify the destination port
 * (0, 1, 2) or broadcast (3) or the source port (1, 2).
 *
 * VID bit 4 is used to specify if the STP port state should be overridden.
 * Required when no forwarding between the external ports should happen.
 */

#define LAN9303_TAG_LEN 4
#define LAN9303_MAX_PORTS 3

static struct sk_buff *lan9303_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct dsa_slave_priv *p = netdev_priv(dev);
	u16 *lan9303_tag;

	/* insert a special VLAN tag between the MAC addresses
	 * and the current ethertype field.
	 */
	if (skb_cow_head(skb, LAN9303_TAG_LEN) < 0) {
		dev_dbg(&dev->dev,
			"Cannot make room for the special tag. Dropping packet\n");
		goto out_free;
	}

	/* provide 'LAN9303_TAG_LEN' bytes additional space */
	skb_push(skb, LAN9303_TAG_LEN);

	/* make room between MACs and Ether-Type */
	memmove(skb->data, skb->data + LAN9303_TAG_LEN, 2 * ETH_ALEN);

	lan9303_tag = (u16 *)(skb->data + 2 * ETH_ALEN);
	lan9303_tag[0] = htons(ETH_P_8021Q);
	lan9303_tag[1] = htons(p->dp->index | BIT(4));

	return skb;
out_free:
	kfree_skb(skb);
	return NULL;
}

static int lan9303_rcv(struct sk_buff *skb, struct net_device *dev,
		       struct packet_type *pt, struct net_device *orig_dev)
{
	u16 *lan9303_tag;
	struct dsa_switch_tree *dst = dev->dsa_ptr;
	struct dsa_switch *ds;
	unsigned int source_port;

	if (unlikely(!dst)) {
		dev_warn_ratelimited(&dev->dev, "Dropping packet, due to missing switch tree device\n");
		goto out_drop;
	}

	ds = dst->ds[0];

	if (unlikely(!ds)) {
		dev_warn_ratelimited(&dev->dev, "Dropping packet, due to missing DSA switch device\n");
		goto out_drop;
	}

	skb = skb_unshare(skb, GFP_ATOMIC);
	if (!skb) {
		dev_warn_ratelimited(&dev->dev, "Cannot post-process skb: unshareable\n");
		goto out;
	}

	if (unlikely(!pskb_may_pull(skb, 2 + 2))) {
		dev_warn_ratelimited(&dev->dev,
				     "Dropping packet, cannot pull\n");
		goto out_drop;
	}

	/* '->data' points into the middle of our special VLAN tag information:
	 *
	 * ~ MAC src   | 0x81 | 0x00 | 0xyy | 0xzz | ether type
	 *                           ^
	 *                        ->data
	 */
	lan9303_tag = (u16 *)(skb->data - 2);

	if (lan9303_tag[0] != htons(ETH_P_8021Q)) {
		dev_warn_ratelimited(&dev->dev, "Dropping packet due to invalid VLAN marker\n");
		goto out_drop;
	}

	source_port = ntohs(lan9303_tag[1]) & 0x3;

	if (source_port >= LAN9303_MAX_PORTS) {
		dev_warn_ratelimited(&dev->dev, "Dropping packet due to invalid source port\n");
		goto out_drop;
	}

	if (!ds->ports[source_port].netdev) {
		dev_warn_ratelimited(&dev->dev, "Dropping packet due to invalid netdev or device\n");
		goto out_drop;
	}

	/* remove the special VLAN tag between the MAC addresses
	 * and the current ethertype field.
	 */
	skb_pull_rcsum(skb, 2 + 2);
	memmove(skb->data - ETH_HLEN, skb->data - (ETH_HLEN + LAN9303_TAG_LEN),
		2 * ETH_ALEN);

	/* Update skb & forward the packet to the dedicated interface */
	skb_push(skb, ETH_HLEN);
	skb->dev = ds->ports[source_port].netdev;
	skb->pkt_type = PACKET_HOST;
	skb->protocol = eth_type_trans(skb, skb->dev);

	skb->dev->stats.rx_packets++;
	skb->dev->stats.rx_bytes += skb->len;

	netif_receive_skb(skb);

	return 0;
out_drop:
	kfree_skb(skb);
out:
	return 0;
}

const struct dsa_device_ops lan9303_netdev_ops = {
	.xmit = lan9303_xmit,
	.rcv = lan9303_rcv,
};
