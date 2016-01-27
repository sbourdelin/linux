/*
 * sctp_offload - GRO/GSO Offloading for SCTP
 *
 * Copyright (C) 2015, Marcelo Ricardo Leitner <marcelo.leitner@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/socket.h>
#include <linux/sctp.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/module.h>
#include <linux/kfifo.h>
#include <linux/time.h>
#include <net/net_namespace.h>

#include <linux/skbuff.h>
#include <net/sctp/sctp.h>
#include <net/sctp/checksum.h>
#include <net/protocol.h>

static __le32 sctp_gso_make_checksum(struct sk_buff *skb)
{
	skb->ip_summed = CHECKSUM_NONE;
	return sctp_compute_cksum(skb, skb_transport_offset(skb));
}

static struct sk_buff *sctp_gso_segment(struct sk_buff *skb,
					netdev_features_t features)
{
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	struct sctphdr *sh;

	sh = sctp_hdr(skb);
	if (!pskb_may_pull(skb, sizeof(*sh)))
		goto out;

	__skb_pull(skb, sizeof(*sh));

	if (skb_gso_ok(skb, features | NETIF_F_GSO_ROBUST)) {
		/* Packet is from an untrusted source, reset gso_segs. */
		int type = skb_shinfo(skb)->gso_type;

		if (unlikely(type &
			     ~(SKB_GSO_SCTP | SKB_GSO_DODGY |
			       0) ||
			     !(type & (SKB_GSO_SCTP))))
			goto out;

		/* This should not happen as no NIC has SCTP GSO
		 * offloading, it's always via software and thus we
		 * won't send a large packet down the stack.
		 */
		WARN_ONCE(1, "SCTP segmentation offloading to NICs is not supported.");
		goto out;
	}

	segs = skb_segment(skb, features);
	if (IS_ERR(segs))
		goto out;

	/* All that is left is update SCTP CRC if necessary */
	for (skb = segs; skb; skb = skb->next) {
		if (skb->ip_summed != CHECKSUM_PARTIAL) {
			sh = sctp_hdr(skb);
			sh->checksum = sctp_gso_make_checksum(skb);
		}
	}

out:
	return segs;
}

static const struct net_offload sctp_offload = {
	.callbacks = {
		.gso_segment = sctp_gso_segment,
	},
};

int __init sctp_offload_init(void)
{
	return inet_add_offload(&sctp_offload, IPPROTO_SCTP);
}
