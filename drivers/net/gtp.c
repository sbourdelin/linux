/* GTP according to GSM TS 09.60 / 3GPP TS 29.060
 *
 * (C) 2012-2014 by sysmocom - s.f.m.c. GmbH
 * (C) 2016 by Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * Author: Harald Welte <hwelte@sysmocom.de>
 *	   Pablo Neira Ayuso <pablo@netfilter.org>
 *	   Andreas Schultz <aschultz@travelping.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/udp.h>
#include <linux/rculist.h>
#include <linux/jhash.h>
#include <linux/if_tunnel.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/file.h>
#include <linux/gtp.h>

#include <net/net_namespace.h>
#include <net/protocol.h>
#include <net/ip.h>
#include <net/ip6_tunnel.h>
#include <net/udp.h>
#include <net/udp_tunnel.h>
#include <net/icmp.h>
#include <net/xfrm.h>
#include <net/genetlink.h>
#include <net/netns/generic.h>
#include <net/gtp.h>

/* An active session for the subscriber. */
struct pdp_ctx {
	struct hlist_node	hlist_tid;
	struct hlist_node	hlist_addr;

	union {
		u64		tid;
		struct {
			u64	tid;
			u16	flow;
		} v0;
		struct {
			u32	i_tei;
			u32	o_tei;
		} v1;
	} u;
	u8			gtp_version;
	__be16			gtp_port;

	u16			ms_af;
	union {
		struct in_addr	ms_addr_ip4;
		struct in6_addr	ms_addr_ip6;
	};

	u16			peer_af;
	union {
		struct in_addr	peer_addr_ip4;
		struct in6_addr	peer_addr_ip6;
	};

	struct sock		*sk;
	struct net_device       *dev;

	atomic_t		tx_seq;
	struct rcu_head		rcu_head;

	struct dst_cache	dst_cache;

	unsigned int		cfg_flags;

#define GTP_F_UDP_ZERO_CSUM_TX		0x1
#define GTP_F_UDP_ZERO_CSUM6_TX		0x2
#define GTP_F_UDP_ZERO_CSUM6_RX		0x4

};

/* One instance of the GTP device. */
struct gtp_dev {
	struct list_head	list;

	unsigned int		is_ipv6:1;

	struct sock		*sk0;
	struct sock		*sk1u;

	struct socket		*sock0;
	struct socket		*sock1u;

	struct net		*net;
	struct net_device	*dev;

	unsigned int		role;
	unsigned int		hash_size;
	struct hlist_head	*tid_hash;

	struct hlist_head	*addr4_hash;
	struct hlist_head	*addr6_hash;

	struct gro_cells	gro_cells;
};

static unsigned int gtp_net_id __read_mostly;

struct gtp_net {
	struct list_head gtp_dev_list;
};

static u32 gtp_h_initval;

static void pdp_context_delete(struct pdp_ctx *pctx);

static int gtp_gso_type;

static inline u32 gtp0_hashfn(u64 tid)
{
	u32 *tid32 = (u32 *) &tid;

	return jhash_2words(tid32[0], tid32[1], gtp_h_initval);
}

static inline u32 gtp1u_hashfn(u32 tid)
{
	return jhash_1word(tid, gtp_h_initval);
}

static inline u32 ipv4_hashfn(__be32 ip)
{
	return jhash_1word((__force u32)ip, gtp_h_initval);
}

static inline u32 ipv6_hashfn(const struct in6_addr *a)
{
	return __ipv6_addr_jhash(a, gtp_h_initval);
}

/* Resolve a PDP context structure based on the 64bit TID. */
static struct pdp_ctx *gtp0_pdp_find(struct gtp_dev *gtp, u64 tid)
{
	struct hlist_head *head;
	struct pdp_ctx *pdp;

	head = &gtp->tid_hash[gtp0_hashfn(tid) % gtp->hash_size];

	hlist_for_each_entry_rcu(pdp, head, hlist_tid) {
		if (pdp->gtp_version == GTP_V0 &&
		    pdp->u.v0.tid == tid)
			return pdp;
	}
	return NULL;
}

/* Resolve a PDP context structure based on the 32bit TEI. */
static struct pdp_ctx *gtp1_pdp_find(struct gtp_dev *gtp, u32 tid)
{
	struct hlist_head *head;
	struct pdp_ctx *pdp;

	head = &gtp->tid_hash[gtp1u_hashfn(tid) % gtp->hash_size];

	hlist_for_each_entry_rcu(pdp, head, hlist_tid) {
		if (pdp->gtp_version == GTP_V1 &&
		    pdp->u.v1.i_tei == tid)
			return pdp;
	}
	return NULL;
}

/* Resolve a PDP context based on IPv4 address of MS. */
static struct pdp_ctx *ipv4_pdp_find(struct gtp_dev *gtp, __be32 ms_addr)
{
	struct hlist_head *head;
	struct pdp_ctx *pdp;

	head = &gtp->addr4_hash[ipv4_hashfn(ms_addr) % gtp->hash_size];

	hlist_for_each_entry_rcu(pdp, head, hlist_addr) {
		if (pdp->ms_af == AF_INET &&
		    pdp->ms_addr_ip4.s_addr == ms_addr)
			return pdp;
	}

	return NULL;
}

static bool gtp_check_ms_ipv4(struct sk_buff *skb, struct pdp_ctx *pctx,
				  unsigned int hdrlen, unsigned int role)
{
	struct iphdr *iph;

	if (!pskb_may_pull(skb, hdrlen + sizeof(struct iphdr)))
		return false;

	iph = (struct iphdr *)(skb->data + hdrlen);

	if (role == GTP_ROLE_SGSN)
		return iph->daddr == pctx->ms_addr_ip4.s_addr;
	else
		return iph->saddr == pctx->ms_addr_ip4.s_addr;
}

/* Resolve a PDP context based on IPv6 address of MS. */
static struct pdp_ctx *ipv6_pdp_find(struct gtp_dev *gtp,
				     const struct in6_addr *ms_addr)
{
	struct hlist_head *head;
	struct pdp_ctx *pdp;

	head = &gtp->addr6_hash[ipv6_hashfn(ms_addr) % gtp->hash_size];

	hlist_for_each_entry_rcu(pdp, head, hlist_addr) {
		if (pdp->ms_af == AF_INET6 &&
		    ipv6_addr_equal(&pdp->ms_addr_ip6, ms_addr))
			return pdp;
	}

	return NULL;
}

static bool gtp_check_ms_ipv6(struct sk_buff *skb, struct pdp_ctx *pctx,
			      unsigned int hdrlen, unsigned int role)
{
	struct ipv6hdr *ipv6h;

	if (!pskb_may_pull(skb, hdrlen + sizeof(struct ipv6hdr)))
		return false;

	ipv6h = (struct ipv6hdr *)(skb->data + hdrlen);

	if (role == GTP_ROLE_SGSN)
		return ipv6_addr_equal(&ipv6h->daddr, &pctx->ms_addr_ip6);
	else
		return ipv6_addr_equal(&ipv6h->saddr, &pctx->ms_addr_ip6);
}

/* Check if the inner IP address in this packet is assigned to any
 * existing mobile subscriber.
 */
static bool gtp_check_ms(struct sk_buff *skb, struct pdp_ctx *pctx,
			     unsigned int hdrlen, unsigned int role)
{
	struct iphdr *iph;

	/* Minimally there needs to be an IPv4 header */
	if (!pskb_may_pull(skb, hdrlen + sizeof(struct iphdr)))
		return false;

	iph = (struct iphdr *)(skb->data + hdrlen);

	switch (iph->version) {
	case 4:
		return gtp_check_ms_ipv4(skb, pctx, hdrlen, role);
	case 6:
		return gtp_check_ms_ipv6(skb, pctx, hdrlen, role);
	}

	return false;
}

static u16 ipver_to_eth(struct iphdr *iph)
{
	switch (iph->version) {
	case 4:
		return htons(ETH_P_IP);
	case 6:
		return htons(ETH_P_IPV6);
	default:
		return 0;
	}
}

static int gtp_rx(struct pdp_ctx *pctx, struct sk_buff *skb,
		  unsigned int hdrlen, unsigned int role)
{
	struct gtp_dev *gtp = netdev_priv(pctx->dev);
	struct pcpu_sw_netstats *stats;
	u16 inner_protocol;

	if (!gtp_check_ms(skb, pctx, hdrlen, role)) {
		netdev_dbg(pctx->dev, "No PDP ctx for this MS\n");
		return 1;
	}

	inner_protocol = ipver_to_eth((struct iphdr *)(skb->data + hdrlen));
	if (!inner_protocol)
		return -1;

	/* Get rid of the GTP + UDP headers. */
	if (iptunnel_pull_header(skb, hdrlen, inner_protocol,
				 !net_eq(gtp->net, dev_net(pctx->dev))))
		return -1;

	netdev_dbg(pctx->dev, "forwarding packet from GGSN to uplink\n");

	/* Now that the UDP and the GTP header have been removed, set up the
	 * new network header. This is required by the upper layer to
	 * calculate the transport header.
	 */
	skb_reset_network_header(skb);

	skb->dev = pctx->dev;

	stats = this_cpu_ptr(pctx->dev->tstats);
	u64_stats_update_begin(&stats->syncp);
	stats->rx_packets++;
	stats->rx_bytes += skb->len;
	u64_stats_update_end(&stats->syncp);

	gro_cells_receive(&gtp->gro_cells, skb);

	return 0;
}

/* UDP encapsulation receive handler for GTPv0-U . See net/ipv4/udp.c.
 * Return codes: 0: success, <0: error, >0: pass up to userspace UDP socket.
 */
static int gtp0_udp_encap_recv(struct sock *sk, struct sk_buff *skb)
{
	struct gtp_dev *gtp = rcu_dereference_sk_user_data(sk);
	unsigned int hdrlen = sizeof(struct udphdr) +
			      sizeof(struct gtp0_header);
	struct gtp0_header *gtp0;
	struct pdp_ctx *pctx;

	if (!gtp)
		goto pass;

	/* Pull through IP header since gtp_rx looks at IP version */
	if (!pskb_may_pull(skb, hdrlen + sizeof(struct iphdr)))
		goto drop;

	gtp0 = (struct gtp0_header *)(skb->data + sizeof(struct udphdr));

	if ((gtp0->flags >> 5) != GTP_V0)
		goto pass;

	if (gtp0->type != GTP_TPDU)
		goto pass;

	netdev_dbg(gtp->dev, "received GTP0 packet\n");

	pctx = gtp0_pdp_find(gtp, be64_to_cpu(gtp0->tid));
	if (!pctx) {
		netdev_dbg(gtp->dev, "No PDP ctx to decap skb=%p\n", skb);
		goto pass;
	}

	if (!gtp_rx(pctx, skb, hdrlen, gtp->role)) {
		/* Successfully received */
		return 0;
	}

drop:
	kfree_skb(skb);
	return 0;

pass:
	return 1;
}

/* UDP encapsulation receive handler for GTPv0-U . See net/ipv4/udp.c.
 * Return codes: 0: success, <0: error, >0: pass up to userspace UDP socket.
 */
static int gtp1u_udp_encap_recv(struct sock *sk, struct sk_buff *skb)
{
	struct gtp_dev *gtp = rcu_dereference_sk_user_data(sk);
	unsigned int hdrlen = sizeof(struct udphdr) +
			      sizeof(struct gtp1_header);
	struct gtp1_header *gtp1;
	struct pdp_ctx *pctx;

	if (!gtp)
		goto pass;

	/* Pull through IP header since gtp_rx looks at IP version */
	if (!pskb_may_pull(skb, hdrlen + sizeof(struct iphdr)))
		goto drop;

	gtp1 = (struct gtp1_header *)(skb->data + sizeof(struct udphdr));

	if ((gtp1->flags >> 5) != GTP_V1)
		goto pass;

	if (gtp1->type != GTP_TPDU)
		goto pass;

	netdev_dbg(gtp->dev, "received GTP1 packet\n");

	/* From 29.060: "This field shall be present if and only if any one or
	 * more of the S, PN and E flags are set.".
	 *
	 * If any of the bit is set, then the remaining ones also have to be
	 * set.
	 */
	if (gtp1->flags & GTP1_F_MASK)
		hdrlen += 4;

	/* Make sure the header is larger enough, including extensions and
	 * also an IP header since gtp_rx looks at IP version
	 */
	if (!pskb_may_pull(skb, hdrlen + sizeof(struct iphdr)))
		goto drop;

	gtp1 = (struct gtp1_header *)(skb->data + sizeof(struct udphdr));

	pctx = gtp1_pdp_find(gtp, ntohl(gtp1->tid));
	if (!pctx) {
		netdev_dbg(gtp->dev, "No PDP ctx to decap skb=%p\n", skb);
		goto pass;
	}

	if (!gtp_rx(pctx, skb, hdrlen, gtp->role)) {
		/* Successfully received */
		return 0;
	}

drop:
	kfree_skb(skb);
	return 0;

pass:
	return 1;
}

static struct sk_buff *gtp_gso_segment(struct sk_buff *skb,
				       netdev_features_t features)
{
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	int tnl_hlen = skb->mac_len;
	struct gtp0_header *gtp0;

	if (unlikely(!pskb_may_pull(skb, tnl_hlen)))
		return ERR_PTR(-EINVAL);

	/* Make sure we have a mininal GTP header */
	if (unlikely(tnl_hlen < min_t(size_t, sizeof(struct gtp0_header),
				      sizeof(struct gtp1_header))))
		return ERR_PTR(-EINVAL);

	/* Determine version */
	gtp0 = (struct gtp0_header *)skb->data;
	switch (gtp0->flags >> 5) {
	case GTP_V0: {
		u16 tx_seq;

		if (unlikely(tnl_hlen != sizeof(struct gtp0_header)))
			return ERR_PTR(-EINVAL);

		tx_seq = ntohs(gtp0->seq);

		/* segment inner packet. */
		segs = skb_mac_gso_segment(skb, features);
		if (!IS_ERR_OR_NULL(segs)) {
			skb = segs;
			do {
				gtp0 = (struct gtp0_header *)
						skb_mac_header(skb);
				gtp0->length = ntohs(skb->len - tnl_hlen);
				gtp0->seq = htons(tx_seq);
				tx_seq++;
			} while ((skb = skb->next));
		}
		break;
	}
	case GTP_V1: {
		struct gtp1_header *gtp1;

		if (unlikely(tnl_hlen != sizeof(struct gtp1_header)))
			return ERR_PTR(-EINVAL);

		/* segment inner packet. */
		segs = skb_mac_gso_segment(skb, features);
		if (!IS_ERR_OR_NULL(segs)) {
			skb = segs;
			do {
				gtp1 = (struct gtp1_header *)
						skb_mac_header(skb);
				gtp1->length = ntohs(skb->len - tnl_hlen);
			} while ((skb = skb->next));
		}
		break;
	}
	}

	return segs;
}

static struct sk_buff **gtp_gro_receive_finish(struct sock *sk,
					       struct sk_buff **head,
					       struct sk_buff *skb,
					       void *hdr, size_t hdrlen)
{
	const struct packet_offload *ptype;
	struct sk_buff **pp;
	__be16 type;

	type = ipver_to_eth((struct iphdr *)((void *)hdr + hdrlen));
	if (!type)
		goto out_err;

	rcu_read_lock();

	ptype = gro_find_receive_by_type(type);
	if (!ptype)
		goto out_unlock_err;

	skb_gro_pull(skb, hdrlen);
	skb_gro_postpull_rcsum(skb, hdr, hdrlen);
	pp = call_gro_receive(ptype->callbacks.gro_receive, head, skb);

	rcu_read_unlock();

	return pp;

out_unlock_err:
	rcu_read_unlock();
out_err:
	NAPI_GRO_CB(skb)->flush |= 1;
	return NULL;
}

static struct sk_buff **gtp0_gro_receive(struct sock *sk,
					 struct sk_buff **head,
					 struct sk_buff *skb)
{
	struct gtp0_header *gtp0;
	size_t len, hdrlen, off;
	struct sk_buff *p;

	off = skb_gro_offset(skb);
	len = off + sizeof(*gtp0);
	hdrlen = sizeof(*gtp0);

	gtp0 = skb_gro_header_fast(skb, off);
	if (skb_gro_header_hard(skb, len)) {
		gtp0 = skb_gro_header_slow(skb, len, off);
		if (unlikely(!gtp0))
			goto out;
	}

	if ((gtp0->flags >> 5) != GTP_V0 || gtp0->type != GTP_TPDU)
		goto out;

	hdrlen += sizeof(*gtp0);

	/* To get IP version */
	len += sizeof(struct iphdr);

	/* Now get header with GTP header an IPv4 header (for version) */
	if (skb_gro_header_hard(skb, len)) {
		gtp0 = skb_gro_header_slow(skb, len, off);
		if (unlikely(!gtp0))
			goto out;
	}

	for (p = *head; p; p = p->next) {
		const struct gtp0_header *gtp0_t;

		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		gtp0_t = (struct gtp0_header *)(p->data + off);

		if (gtp0->flags != gtp0_t->flags ||
		    gtp0->type != gtp0_t->type ||
		    gtp0->flow != gtp0_t->flow ||
		    gtp0->tid != gtp0_t->tid) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}
	}

	return gtp_gro_receive_finish(sk, head, skb, gtp0, hdrlen);

out:
	NAPI_GRO_CB(skb)->flush |= 1;

	return NULL;
}

static struct sk_buff **gtp1u_gro_receive(struct sock *sk,
					  struct sk_buff **head,
					  struct sk_buff *skb)
{
	struct gtp1_header *gtp1;
	size_t len, hdrlen, off;
	struct sk_buff *p;

	off = skb_gro_offset(skb);
	len = off + sizeof(*gtp1);
	hdrlen = sizeof(*gtp1);

	gtp1 = skb_gro_header_fast(skb, off);
	if (skb_gro_header_hard(skb, len)) {
		gtp1 = skb_gro_header_slow(skb, len, off);
		if (unlikely(!gtp1))
			goto out;
	}

	if ((gtp1->flags >> 5) != GTP_V1 || gtp1->type != GTP_TPDU)
		goto out;

	if (gtp1->flags & GTP1_F_MASK) {
		hdrlen += 4;
		len += 4;
	}

	len += sizeof(struct iphdr);

	/* Now get header with GTP header an IPv4 header (for version) */
	if (skb_gro_header_hard(skb, len)) {
		gtp1 = skb_gro_header_slow(skb, len, off);
		if (unlikely(!gtp1))
			goto out;
	}

	for (p = *head; p; p = p->next) {
		const struct gtp1_header *gtp1_t;

		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		gtp1_t = (struct gtp1_header *)(p->data + off);

		if (gtp1->flags != gtp1_t->flags ||
		    gtp1->type != gtp1_t->type ||
		    gtp1->tid != gtp1_t->tid) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}
	}

	return gtp_gro_receive_finish(sk, head, skb, gtp1, hdrlen);

out:
	NAPI_GRO_CB(skb)->flush = 1;

	return NULL;
}

static int gtp_gro_complete_finish(struct sock *sk, struct sk_buff *skb,
				   int nhoff, size_t hdrlen)
{
	struct packet_offload *ptype;
	int err = -EINVAL;
	__be16 type;

	type = ipver_to_eth((struct iphdr *)(skb->data + nhoff + hdrlen));
	if (!type)
		return err;

	rcu_read_lock();
	ptype = gro_find_complete_by_type(type);
	if (ptype)
		err = ptype->callbacks.gro_complete(skb, nhoff + hdrlen);

	rcu_read_unlock();

	skb_set_inner_mac_header(skb, nhoff + hdrlen);

	return err;
}

static int gtp0_gro_complete(struct sock *sk, struct sk_buff *skb, int nhoff)
{
	struct gtp0_header *gtp0 = (struct gtp0_header *)(skb->data + nhoff);
	size_t hdrlen = sizeof(struct gtp0_header);

	gtp0->length = htons(skb->len - nhoff - hdrlen);

	return gtp_gro_complete_finish(sk, skb, nhoff, hdrlen);
}

static int gtp1u_gro_complete(struct sock *sk, struct sk_buff *skb, int nhoff)
{
	struct gtp1_header *gtp1 = (struct gtp1_header *)(skb->data + nhoff);
	size_t hdrlen = sizeof(struct gtp1_header);

	if (gtp1->flags & GTP1_F_MASK)
		hdrlen += 4;

	gtp1->length = htons(skb->len - nhoff - hdrlen);

	return gtp_gro_complete_finish(sk, skb, nhoff, hdrlen);
}

static void gtp_encap_destroy(struct sock *sk)
{
	struct gtp_dev *gtp;

	gtp = rcu_dereference_sk_user_data(sk);
	if (gtp) {
		udp_sk(sk)->encap_type = 0;
		rcu_assign_sk_user_data(sk, NULL);
		sock_put(sk);
	}
}

static void gtp_encap_release(struct gtp_dev *gtp)
{
	if (gtp->sk0) {
		if (gtp->sock0) {
			udp_tunnel_sock_release(gtp->sock0);
			gtp->sock0 = NULL;
		} else {
			gtp_encap_destroy(gtp->sk0);
		}

		gtp->sk0 = NULL;
	}

	if (gtp->sk1u) {
		if (gtp->sock1u) {
			udp_tunnel_sock_release(gtp->sock1u);
			gtp->sock1u = NULL;
		} else {
			gtp_encap_destroy(gtp->sk1u);
		}

		gtp->sk1u = NULL;
	}
}

static int gtp_dev_init(struct net_device *dev)
{
	dev->tstats = netdev_alloc_pcpu_stats(struct pcpu_sw_netstats);
	if (!dev->tstats)
		return -ENOMEM;

	return 0;
}

static void gtp_dev_uninit(struct net_device *dev)
{
	struct gtp_dev *gtp = netdev_priv(dev);

	gtp_encap_release(gtp);

	free_percpu(dev->tstats);
}

static inline void gtp0_push_header(struct sk_buff *skb, struct pdp_ctx *pctx)
{
	int payload_len = skb->len;
	struct gtp0_header *gtp0;
	u32 tx_seq;

	gtp0 = skb_push(skb, sizeof(*gtp0));

	gtp0->flags	= 0x1e; /* v0, GTP-non-prime. */
	gtp0->type	= GTP_TPDU;
	gtp0->length	= htons(payload_len);
	gtp0->flow	= htons(pctx->u.v0.flow);
	gtp0->number	= 0xff;
	gtp0->spare[0]	= gtp0->spare[1] = gtp0->spare[2] = 0xff;
	gtp0->tid	= cpu_to_be64(pctx->u.v0.tid);

	/* If skb is GSO allocate sequence numbers for all the segments */
	tx_seq = skb_shinfo(skb)->gso_segs ?
			atomic_add_return(skb_shinfo(skb)->gso_segs,
					  &pctx->tx_seq) :
			atomic_inc_return(&pctx->tx_seq);

	gtp0->seq	= (htons((u16)tx_seq) - 1) & 0xffff;
}

static inline void gtp1_push_header(struct sk_buff *skb, struct pdp_ctx *pctx)
{
	int payload_len = skb->len;
	struct gtp1_header *gtp1;

	gtp1 = skb_push(skb, sizeof(*gtp1));

	/* Bits    8  7  6  5  4  3  2	1
	 *	  +--+--+--+--+--+--+--+--+
	 *	  |version |PT| 0| E| S|PN|
	 *	  +--+--+--+--+--+--+--+--+
	 *	    0  0  1  1	1  0  0  0
	 */
	gtp1->flags	= 0x30; /* v1, GTP-non-prime. */
	gtp1->type	= GTP_TPDU;
	gtp1->length	= htons(payload_len);
	gtp1->tid	= htonl(pctx->u.v1.o_tei);

	/* TODO: Suppport for extension header, sequence number and N-PDU.
	 *	 Update the length field if any of them is available.
	 */
}

static void gtp_push_header(struct sk_buff *skb, struct pdp_ctx *pctx)
{
	switch (pctx->gtp_version) {
	case GTP_V0:
		gtp0_push_header(skb, pctx);
		break;
	case GTP_V1:
		gtp1_push_header(skb, pctx);
		break;
	}
}

static size_t gtp_max_header_len(int version)

{
	switch (version) {
	case GTP_V0:
		return sizeof(struct gtp0_header);
	case GTP_V1:
		return sizeof(struct gtp1_header) + 4;
	}

	/* Should not happen */
	return 0;
}

static int gtp_build_skb(struct sk_buff *skb, struct dst_entry *dst,
			 struct pdp_ctx *pctx, bool xnet, int ip_hdr_len,
			 bool udp_sum)
{
	int type = (udp_sum ? SKB_GSO_UDP_TUNNEL_CSUM : SKB_GSO_UDP_TUNNEL) |
		   gtp_gso_type;
	int min_headroom;
	u16 protocol;
	int err;

	skb_scrub_packet(skb, xnet);

	min_headroom = LL_RESERVED_SPACE(dst->dev) + dst->header_len +
		       gtp_max_header_len(pctx->gtp_version) + ip_hdr_len;

	err = skb_cow_head(skb, min_headroom);
	if (unlikely(err))
		goto free_dst;

	err = iptunnel_handle_offloads(skb, type);
	if (err)
		goto free_dst;

	protocol = ipver_to_eth(ip_hdr(skb));

	gtp_push_header(skb, pctx);

	/* GTP header is treated as inner MAC header */
	skb_reset_inner_mac_header(skb);

	skb_set_inner_protocol(skb, protocol);

	return 0;

free_dst:
	dst_release(dst);
	return err;
}

static int gtp_xmit(struct sk_buff *skb, struct net_device *dev,
		    struct pdp_ctx *pctx)
{
	struct gtp_dev *gtp = netdev_priv(dev);
	bool xnet = !net_eq(gtp->net, dev_net(gtp->dev));
	struct sock *sk = pctx->sk;
	bool udp_csum;
	int err = 0;

	if (pctx->peer_af == AF_INET) {
		__be32 saddr = inet_sk(sk)->inet_saddr;
		struct rtable *rt;

		rt = ip_tunnel_get_route(dev, skb, sk->sk_protocol,
					 sk->sk_bound_dev_if, RT_CONN_FLAGS(sk),
					 pctx->peer_addr_ip4.s_addr, &saddr,
					 pctx->gtp_port, pctx->gtp_port,
					 &pctx->dst_cache, NULL);

		if (IS_ERR(rt)) {
			err = PTR_ERR(rt);
			goto out_err;
		}

		err = gtp_build_skb(skb, &rt->dst, pctx, xnet,
				    sizeof(struct iphdr),
				    !(pctx->cfg_flags &
				      GTP_F_UDP_ZERO_CSUM_TX));
		if (err)
			goto out_err;

		udp_csum = !(pctx->cfg_flags & GTP_F_UDP_ZERO_CSUM_TX);
		udp_tunnel_xmit_skb(rt, sk, skb, saddr,
				    pctx->peer_addr_ip4.s_addr,
				    0, ip4_dst_hoplimit(&rt->dst), 0,
				    pctx->gtp_port, pctx->gtp_port,
				    xnet, !udp_csum);

		netdev_dbg(dev, "gtp -> IP src: %pI4 dst: %pI4\n",
			   &saddr, &pctx->peer_addr_ip4.s_addr);

#if IS_ENABLED(CONFIG_IPV6)
	} else if (pctx->peer_af == AF_INET6) {
		struct in6_addr saddr = inet6_sk(sk)->saddr;
		struct dst_entry *dst;

		dst = ip6_tnl_get_route(dev, skb, sk, sk->sk_protocol,
					sk->sk_bound_dev_if, 0,
					0, &pctx->peer_addr_ip6, &saddr,
					pctx->gtp_port, pctx->gtp_port,
					&pctx->dst_cache, NULL);

		if (IS_ERR(dst)) {
			err = PTR_ERR(dst);
			goto out_err;
		}

		err = gtp_build_skb(skb, dst, pctx, xnet,
				    sizeof(struct ipv6hdr),
				    !(pctx->cfg_flags &
				      GTP_F_UDP_ZERO_CSUM6_TX));
		if (err)
			goto out_err;

		udp_csum = !(pctx->cfg_flags & GTP_F_UDP_ZERO_CSUM6_TX);
		udp_tunnel6_xmit_skb(dst, sk, skb, dev,
				     &saddr, &pctx->peer_addr_ip6,
				     0, ip6_dst_hoplimit(dst), 0,
				     pctx->gtp_port, pctx->gtp_port,
				     !udp_csum);

		netdev_dbg(dev, "gtp -> IP src: %pI6 dst: %pI6\n",
			   &saddr, &pctx->peer_addr_ip6);

#endif
	}

	return 0;

out_err:
	if (err == -ELOOP)
		dev->stats.collisions++;
	else
		dev->stats.tx_carrier_errors++;

	return err;
}

static netdev_tx_t gtp_dev_xmit(struct sk_buff *skb, struct net_device *dev)
{
	unsigned int proto = ntohs(skb->protocol);
	struct gtp_dev *gtp = netdev_priv(dev);
	struct pdp_ctx *pctx;
	int err;

	/* PDP context lookups in gtp_build_skb_*() need rcu read-side lock. */
	rcu_read_lock();
	switch (proto) {
	case ETH_P_IP: {
		struct iphdr *iph = ip_hdr(skb);

		if (gtp->role == GTP_ROLE_SGSN)
			pctx = ipv4_pdp_find(gtp, iph->saddr);
		else
			pctx = ipv4_pdp_find(gtp, iph->daddr);

		if (!pctx) {
			netdev_dbg(dev, "no PDP ctx found for %pI4, skip\n",
				   &iph->daddr);
			err = -ENOENT;
			goto tx_err;
		}

		break;
	}
	case ETH_P_IPV6: {
		struct ipv6hdr *ipv6h = ipv6_hdr(skb);

		if (gtp->role == GTP_ROLE_SGSN)
			pctx = ipv6_pdp_find(gtp, &ipv6h->saddr);
		else
			pctx = ipv6_pdp_find(gtp, &ipv6h->daddr);

		if (!pctx) {
			netdev_dbg(dev, "no PDP ctx found for %pI6, skip\n",
				   &ipv6h->daddr);
			err = -ENOENT;
			goto tx_err;
		}

		break;
	}
	default:
		err = -EOPNOTSUPP;
		goto tx_err;
	}

	netdev_dbg(dev, "found PDP context %p\n", pctx);

	err = gtp_xmit(skb, dev, pctx);

	if (err < 0)
		goto tx_err;

	rcu_read_unlock();

	return NETDEV_TX_OK;

tx_err:
	rcu_read_unlock();
	dev->stats.tx_errors++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops gtp_netdev_ops = {
	.ndo_init		= gtp_dev_init,
	.ndo_uninit		= gtp_dev_uninit,
	.ndo_start_xmit		= gtp_dev_xmit,
	.ndo_get_stats64	= ip_tunnel_get_stats64,
};

#define GTP_FEATURES (NETIF_F_SG |		\
		      NETIF_F_FRAGLIST |	\
		      NETIF_F_HIGHDMA |		\
		      NETIF_F_GSO_SOFTWARE |	\
		      NETIF_F_HW_CSUM)

static void gtp_link_setup(struct net_device *dev)
{
	struct gtp_dev *gtp = netdev_priv(dev);
	dev->netdev_ops		= &gtp_netdev_ops;
	dev->needs_free_netdev	= true;

	dev->hard_header_len = 0;
	dev->addr_len = 0;

	/* Zero header length. */
	dev->type = ARPHRD_NONE;
	dev->flags = IFF_POINTOPOINT | IFF_NOARP | IFF_MULTICAST;

	dev->priv_flags	|= IFF_NO_QUEUE;

	dev->features	|= NETIF_F_LLTX;
	dev->features	|= GTP_FEATURES;

	dev->hw_features |= GTP_FEATURES;
	dev->hw_features |= NETIF_F_GSO_SOFTWARE;

	netif_keep_dst(dev);

	/* Assume largest header, ie. GTPv0. */
	dev->needed_headroom	= LL_MAX_HEADER +
				  max_t(int, sizeof(struct iphdr),
					sizeof(struct ipv6hdr)) +
				  sizeof(struct udphdr) +
				  sizeof(struct gtp0_header);

	gtp->dev = dev;

	gro_cells_init(&gtp->gro_cells, dev);
}

static int gtp_hashtable_new(struct gtp_dev *gtp, int hsize);
static void gtp_hashtable_free(struct gtp_dev *gtp);
static int gtp_encap_enable(struct gtp_dev *gtp, struct nlattr *data[],
			    bool is_ipv6);

static int gtp_newlink(struct net *src_net, struct net_device *dev,
		       struct nlattr *tb[], struct nlattr *data[],
		       struct netlink_ext_ack *extack)
{
	unsigned int role = GTP_ROLE_GGSN;
	bool have_fd, have_ports;
	unsigned int flags = 0;
	bool is_ipv6 = false;
	struct gtp_dev *gtp;
	struct gtp_net *gn;
	int hashsize, err;

	have_fd = !!data[IFLA_GTP_FD0] || !!data[IFLA_GTP_FD1];
	have_ports = !!data[IFLA_GTP_PORT0] || !!data[IFLA_GTP_PORT1];

	if (!(have_fd ^ have_ports)) {
		/* Either got fd(s) or port(s) */
		return -EINVAL;
	}

	if (data[IFLA_GTP_ROLE]) {
		role = nla_get_u32(data[IFLA_GTP_ROLE]);
		if (role > GTP_ROLE_SGSN)
			return -EINVAL;
	}

	if (data[IFLA_GTP_UDP_CSUM]) {
		if (!nla_get_u8(data[IFLA_GTP_UDP_CSUM]))
			flags |= GTP_F_UDP_ZERO_CSUM_TX;
	}

	if (data[IFLA_GTP_UDP_ZERO_CSUM6_TX]) {
		if (nla_get_u8(data[IFLA_GTP_UDP_ZERO_CSUM6_TX]))
			flags |= GTP_F_UDP_ZERO_CSUM6_TX;
	}

	if (data[IFLA_GTP_UDP_ZERO_CSUM6_RX]) {
		if (nla_get_u8(data[IFLA_GTP_UDP_ZERO_CSUM6_RX]))
			flags |= GTP_F_UDP_ZERO_CSUM6_RX;
	}

	if (data[IFLA_GTP_AF]) {
		u16 af = nla_get_u16(data[IFLA_GTP_AF]);

		switch (af) {
		case AF_INET:
			is_ipv6 = false;
			break;
		case AF_INET6:
			is_ipv6 = true;
			break;
		default:
			return -EINVAL;
		}
	}

	gtp = netdev_priv(dev);

	err = gtp_encap_enable(gtp, data, is_ipv6);
	if (err < 0)
		return err;

	if (!data[IFLA_GTP_PDP_HASHSIZE])
		hashsize = 1024;
	else
		hashsize = nla_get_u32(data[IFLA_GTP_PDP_HASHSIZE]);

	err = gtp_hashtable_new(gtp, hashsize);
	if (err < 0)
		goto out_encap;

	err = register_netdevice(dev);
	if (err < 0) {
		netdev_dbg(dev, "failed to register new netdev %d\n", err);
		goto out_hashtable;
	}

	gtp->role = role;
	gtp->is_ipv6 = is_ipv6;
	gtp->net = src_net;

	gn = net_generic(dev_net(dev), gtp_net_id);
	list_add_rcu(&gtp->list, &gn->gtp_dev_list);

	netdev_dbg(dev, "registered new GTP interface\n");

	return 0;

out_hashtable:
	gtp_hashtable_free(gtp);
out_encap:
	gtp_encap_release(gtp);
	return err;
}

static void gtp_dellink(struct net_device *dev, struct list_head *head)
{
	struct gtp_dev *gtp = netdev_priv(dev);

	gro_cells_destroy(&gtp->gro_cells);
	gtp_encap_release(gtp);
	gtp_hashtable_free(gtp);
	list_del_rcu(&gtp->list);
	unregister_netdevice_queue(dev, head);
}

static const struct nla_policy gtp_policy[IFLA_GTP_MAX + 1] = {
	[IFLA_GTP_FD0]			= { .type = NLA_U32 },
	[IFLA_GTP_FD1]			= { .type = NLA_U32 },
	[IFLA_GTP_PDP_HASHSIZE]		= { .type = NLA_U32 },
	[IFLA_GTP_ROLE]			= { .type = NLA_U32 },
	[IFLA_GTP_PORT0]		= { .type = NLA_U16 },
	[IFLA_GTP_PORT1]		= { .type = NLA_U16 },
	[IFLA_GTP_UDP_CSUM]		= { .type = NLA_U8 },
	[IFLA_GTP_UDP_ZERO_CSUM6_TX]	= { .type = NLA_U8 },
	[IFLA_GTP_UDP_ZERO_CSUM6_RX]	= { .type = NLA_U8 },
};

static int gtp_validate(struct nlattr *tb[], struct nlattr *data[],
			struct netlink_ext_ack *extack)
{
	if (!data)
		return -EINVAL;

	return 0;
}

static size_t gtp_get_size(const struct net_device *dev)
{
	return nla_total_size(sizeof(__u32));	/* IFLA_GTP_PDP_HASHSIZE */
}

static int gtp_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct gtp_dev *gtp = netdev_priv(dev);

	if (nla_put_u32(skb, IFLA_GTP_PDP_HASHSIZE, gtp->hash_size))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static struct rtnl_link_ops gtp_link_ops __read_mostly = {
	.kind		= "gtp",
	.maxtype	= IFLA_GTP_MAX,
	.policy		= gtp_policy,
	.priv_size	= sizeof(struct gtp_dev),
	.setup		= gtp_link_setup,
	.validate	= gtp_validate,
	.newlink	= gtp_newlink,
	.dellink	= gtp_dellink,
	.get_size	= gtp_get_size,
	.fill_info	= gtp_fill_info,
};

static int gtp_hashtable_new(struct gtp_dev *gtp, int hsize)
{
	int i;

	gtp->addr4_hash = kmalloc_array(hsize, sizeof(*gtp->addr4_hash),
					GFP_KERNEL);
	if (!gtp->addr4_hash)
		goto err;

	gtp->addr6_hash = kmalloc_array(hsize, sizeof(*gtp->addr6_hash),
					GFP_KERNEL);
	if (!gtp->addr6_hash)
		goto err;

	gtp->tid_hash = kmalloc_array(hsize, sizeof(struct hlist_head),
				      GFP_KERNEL);
	if (!gtp->tid_hash)
		goto err;

	gtp->hash_size = hsize;

	for (i = 0; i < hsize; i++) {
		INIT_HLIST_HEAD(&gtp->addr4_hash[i]);
		INIT_HLIST_HEAD(&gtp->addr6_hash[i]);
		INIT_HLIST_HEAD(&gtp->tid_hash[i]);
	}
	return 0;
err:
	kfree(gtp->addr4_hash);
	kfree(gtp->addr6_hash);
	return -ENOMEM;
}

static void gtp_hashtable_free(struct gtp_dev *gtp)
{
	struct pdp_ctx *pctx;
	int i;

	for (i = 0; i < gtp->hash_size; i++)
		hlist_for_each_entry_rcu(pctx, &gtp->tid_hash[i], hlist_tid)
			pdp_context_delete(pctx);

	synchronize_rcu();
	kfree(gtp->addr4_hash);
	kfree(gtp->addr6_hash);
	kfree(gtp->tid_hash);
}

static int gtp_encap_enable_sock(struct socket *sock, int type,
				 struct gtp_dev *gtp)
{
	struct udp_tunnel_sock_cfg tuncfg = {NULL};

	switch (type) {
	case UDP_ENCAP_GTP0:
		tuncfg.encap_rcv = gtp0_udp_encap_recv;
		tuncfg.gro_receive = gtp0_gro_receive;
		tuncfg.gro_complete = gtp0_gro_complete;
		break;
	case UDP_ENCAP_GTP1U:
		tuncfg.encap_rcv = gtp1u_udp_encap_recv;
		tuncfg.gro_receive = gtp1u_gro_receive;
		tuncfg.gro_complete = gtp1u_gro_complete;
		break;
	default:
		pr_debug("Unknown encap type %u\n", type);
		return -EINVAL;
	}

	tuncfg.sk_user_data = gtp;
	tuncfg.encap_type = type;
	tuncfg.encap_destroy = gtp_encap_destroy;

	setup_udp_tunnel_sock(sock_net(sock->sk), sock, &tuncfg);

	return 0;
}

static struct sock *gtp_encap_enable_fd(int fd, int type, struct gtp_dev *gtp,
					bool is_ipv6)
{
	struct socket *sock;
	struct sock *sk;
	int err;

	pr_debug("enable gtp on %d, %d\n", fd, type);

	sock = sockfd_lookup(fd, &err);
	if (!sock) {
		pr_debug("gtp socket fd=%d not found\n", fd);
		return NULL;
	}

	if (sock->sk->sk_protocol != IPPROTO_UDP) {
		pr_debug("socket fd=%d not UDP\n", fd);
		sk = ERR_PTR(-EINVAL);
		goto out_sock;
	}

	if (sock->sk->sk_family != (is_ipv6 ? AF_INET6 : AF_INET)) {
		pr_debug("socket fd=%d not right family\n", fd);
		sk = ERR_PTR(-EINVAL);
		goto out_sock;
	}

	if (rcu_dereference_sk_user_data(sock->sk)) {
		sk = ERR_PTR(-EBUSY);
		goto out_sock;
	}

	sk = sock->sk;
	sock_hold(sk);

	err = gtp_encap_enable_sock(sock, type, gtp);
	if (err < 0)
		sk = ERR_PTR(err);

out_sock:
	sockfd_put(sock);
	return sk;
}

static struct socket *gtp_create_sock(struct net *net, bool ipv6,
				      __be16 port, u32 flags)
{
	struct socket *sock;
	struct udp_port_cfg udp_conf;
	int err;

	memset(&udp_conf, 0, sizeof(udp_conf));

	if (ipv6) {
		udp_conf.family = AF_INET6;
		udp_conf.use_udp6_rx_checksums =
		    !(flags & GTP_F_UDP_ZERO_CSUM6_RX);
		udp_conf.ipv6_v6only = 1;
	} else {
		udp_conf.family = AF_INET;
	}

	udp_conf.local_udp_port = port;

	/* Open UDP socket */
	err = udp_sock_create(net, &udp_conf, &sock);
	if (err)
		return ERR_PTR(err);

	return sock;
}

static int gtp_encap_enable(struct gtp_dev *gtp, struct nlattr *data[],
			    bool is_ipv6)
{
	int err;

	struct socket *sock0 = NULL, *sock1u = NULL;
	struct sock *sk0 = NULL, *sk1u = NULL;

	if (data[IFLA_GTP_FD0]) {
		u32 fd0 = nla_get_u32(data[IFLA_GTP_FD0]);

		sk0 = gtp_encap_enable_fd(fd0, UDP_ENCAP_GTP0, gtp, is_ipv6);
		if (IS_ERR(sk0)) {
			err = PTR_ERR(sk0);
			sk0 = NULL;
			goto out_err;
		}
	} else if (data[IFLA_GTP_PORT0]) {
		__be16 port = nla_get_u16(data[IFLA_GTP_PORT0]);

		sock0 = gtp_create_sock(dev_net(gtp->dev), is_ipv6, port, 0);
		if (IS_ERR(sock0)) {
			err = PTR_ERR(sock0);
			sock0 = NULL;
			goto out_err;
		}

		err = gtp_encap_enable_sock(sock0, UDP_ENCAP_GTP0, gtp);
		if (err)
			goto out_err;
	}

	if (data[IFLA_GTP_FD1]) {
		u32 fd1 = nla_get_u32(data[IFLA_GTP_FD1]);

		sk1u = gtp_encap_enable_fd(fd1, UDP_ENCAP_GTP1U, gtp, is_ipv6);
		if (IS_ERR(sk1u)) {
			err = PTR_ERR(sk1u);
			sk1u = NULL;
			goto out_err;
		}
	} else if (data[IFLA_GTP_PORT1]) {
		__be16 port = nla_get_u16(data[IFLA_GTP_PORT1]);

		sock1u = gtp_create_sock(dev_net(gtp->dev), is_ipv6, port, 0);
		if (IS_ERR(sock1u)) {
			err = PTR_ERR(sock1u);
			sock1u = NULL;
			goto out_err;
		}

		err = gtp_encap_enable_sock(sock1u, UDP_ENCAP_GTP1U, gtp);
		if (err)
			goto out_err;
	}

	if (sock0) {
		gtp->sock0 = sock0;
		gtp->sk0 = sock0->sk;
	} else {
		gtp->sk0 = sk0;
	}

	if (sock1u) {
		gtp->sock1u = sock1u;
		gtp->sk1u = sock1u->sk;
	} else {
		gtp->sk1u = sk1u;
	}

	return 0;

out_err:
	if (sk0)
		gtp_encap_destroy(sk0);
	if (sk1u)
		gtp_encap_destroy(sk1u);
	if (sock0)
		udp_tunnel_sock_release(sock0);
	if (sock1u)
		udp_tunnel_sock_release(sock1u);

	return err;
}

static struct gtp_dev *gtp_find_dev(struct net *src_net, struct nlattr *nla[])
{
	struct gtp_dev *gtp = NULL;
	struct net_device *dev;
	struct net *net;

	/* Examine the link attributes and figure out which network namespace
	 * we are talking about.
	 */
	if (nla[GTPA_NET_NS_FD])
		net = get_net_ns_by_fd(nla_get_u32(nla[GTPA_NET_NS_FD]));
	else
		net = get_net(src_net);

	if (IS_ERR(net))
		return NULL;

	/* Check if there's an existing gtpX device to configure */
	dev = dev_get_by_index_rcu(net, nla_get_u32(nla[GTPA_LINK]));
	if (dev && dev->netdev_ops == &gtp_netdev_ops)
		gtp = netdev_priv(dev);

	put_net(net);
	return gtp;
}

static void pdp_fill(struct pdp_ctx *pctx, struct genl_info *info)
{
	__be16 default_port = 0;

	pctx->gtp_version = nla_get_u32(info->attrs[GTPA_VERSION]);

	if (info->attrs[GTPA_PEER_ADDRESS]) {
		pctx->peer_af = AF_INET;
		pctx->peer_addr_ip4.s_addr =
			nla_get_in_addr(info->attrs[GTPA_PEER_ADDRESS]);
	} else if (info->attrs[GTPA_PEER6_ADDRESS]) {
		pctx->peer_af = AF_INET6;
		pctx->peer_addr_ip6 = nla_get_in6_addr(
					info->attrs[GTPA_PEER6_ADDRESS]);
	}

	switch (pctx->gtp_version) {
	case GTP_V0:
		/* According to TS 09.60, sections 7.5.1 and 7.5.2, the flow
		 * label needs to be the same for uplink and downlink packets,
		 * so let's annotate this.
		 */
		pctx->u.v0.tid = nla_get_u64(info->attrs[GTPA_TID]);
		pctx->u.v0.flow = nla_get_u16(info->attrs[GTPA_FLOW]);
		default_port = htons(GTP0_PORT);
		break;
	case GTP_V1:
		pctx->u.v1.i_tei = nla_get_u32(info->attrs[GTPA_I_TEI]);
		pctx->u.v1.o_tei = nla_get_u32(info->attrs[GTPA_O_TEI]);
		default_port = htons(GTP1U_PORT);
		break;
	default:
		break;
	}

	if (info->attrs[GTPA_PORT])
		pctx->gtp_port = nla_get_u16(info->attrs[GTPA_PORT]);
	else
		pctx->gtp_port = default_port;
}

static int gtp_pdp_add(struct gtp_dev *gtp, struct sock *sk,
		       struct genl_info *info)
{
	struct net_device *dev = gtp->dev;
	struct hlist_head *addr_list;
	struct pdp_ctx *pctx = NULL;
	u32 hash_ms, hash_tid = 0;
	struct in6_addr ms6_addr;
	__be32 ms_addr = 0;
	int ms_af;
	int err;

	/* Caller ensures we have either v4 or v6 mobile subscriber address */
	if (info->attrs[GTPA_MS_ADDRESS]) {
		/* IPv4 mobile subscriber */

		ms_addr = nla_get_in_addr(info->attrs[GTPA_MS_ADDRESS]);
		hash_ms = ipv4_hashfn(ms_addr) % gtp->hash_size;
		addr_list = &gtp->addr4_hash[hash_ms];
		ms_af = AF_INET;

		pctx = ipv4_pdp_find(gtp, ms_addr);
	} else {
		/* IPv6 mobile subscriber */

		ms6_addr = nla_get_in6_addr(info->attrs[GTPA_MS6_ADDRESS]);
		hash_ms = ipv6_hashfn(&ms6_addr) % gtp->hash_size;
		addr_list = &gtp->addr6_hash[hash_ms];
		ms_af = AF_INET6;

		pctx = ipv6_pdp_find(gtp, &ms6_addr);
	}

	if (pctx) {
		if (info->nlhdr->nlmsg_flags & NLM_F_EXCL)
			return -EEXIST;
		if (info->nlhdr->nlmsg_flags & NLM_F_REPLACE)
			return -EOPNOTSUPP;

		pdp_fill(pctx, info);

		if (pctx->gtp_version == GTP_V0)
			netdev_dbg(dev, "GTPv0-U: update tunnel id = %llx (pdp %p)\n",
				   pctx->u.v0.tid, pctx);
		else if (pctx->gtp_version == GTP_V1)
			netdev_dbg(dev, "GTPv1-U: update tunnel id = %x/%x (pdp %p)\n",
				   pctx->u.v1.i_tei, pctx->u.v1.o_tei, pctx);

		return 0;

	}

	pctx = kmalloc(sizeof(struct pdp_ctx), GFP_KERNEL);
	if (pctx == NULL)
		return -ENOMEM;

	err = dst_cache_init(&pctx->dst_cache, GFP_KERNEL);
	if (err) {
		kfree(pctx);
		return err;
	}

	sock_hold(sk);
	pctx->sk = sk;
	pctx->dev = gtp->dev;
	pctx->ms_af = ms_af;

	switch (ms_af) {
	case AF_INET:
		pctx->ms_addr_ip4.s_addr = ms_addr;
		break;
	case AF_INET6:
		pctx->ms_addr_ip6 = ms6_addr;
		break;
	}

	pdp_fill(pctx, info);
	atomic_set(&pctx->tx_seq, 0);

	switch (pctx->gtp_version) {
	case GTP_V0:
		/* TS 09.60: "The flow label identifies unambiguously a GTP
		 * flow.". We use the tid for this instead, I cannot find a
		 * situation in which this doesn't unambiguosly identify the
		 * PDP context.
		 */
		hash_tid = gtp0_hashfn(pctx->u.v0.tid) % gtp->hash_size;
		break;
	case GTP_V1:
		hash_tid = gtp1u_hashfn(pctx->u.v1.i_tei) % gtp->hash_size;
		break;
	}

	hlist_add_head_rcu(&pctx->hlist_addr, addr_list);
	hlist_add_head_rcu(&pctx->hlist_tid, &gtp->tid_hash[hash_tid]);

	switch (pctx->gtp_version) {
	case GTP_V0:
		netdev_dbg(dev, "GTPv0-U: new PDP ctx id=%llx ssgn=%pI4 ms=%pI4 (pdp=%p)\n",
			   pctx->u.v0.tid, &pctx->peer_addr_ip4,
			   &pctx->ms_addr_ip4, pctx);
		break;
	case GTP_V1:
		netdev_dbg(dev, "GTPv1-U: new PDP ctx id=%x/%x ssgn=%pI4 ms=%pI4 (pdp=%p)\n",
			   pctx->u.v1.i_tei, pctx->u.v1.o_tei,
			   &pctx->peer_addr_ip4, &pctx->ms_addr_ip4, pctx);
		break;
	}

	return 0;
}

static void pdp_context_free(struct rcu_head *head)
{
	struct pdp_ctx *pctx = container_of(head, struct pdp_ctx, rcu_head);

	sock_put(pctx->sk);
	kfree(pctx);
}

static void pdp_context_delete(struct pdp_ctx *pctx)
{
	hlist_del_rcu(&pctx->hlist_tid);
	hlist_del_rcu(&pctx->hlist_addr);
	call_rcu(&pctx->rcu_head, pdp_context_free);
}

static int gtp_genl_new_pdp(struct sk_buff *skb, struct genl_info *info)
{
	unsigned int version;
	struct gtp_dev *gtp;
	struct sock *sk;
	int err;

	if (!info->attrs[GTPA_VERSION] ||
	    !info->attrs[GTPA_LINK])
		return -EINVAL;

	if (!(!!info->attrs[GTPA_PEER_ADDRESS] ^
	      !!info->attrs[GTPA_PEER6_ADDRESS])) {
		/* Either v4 or v6 peer address must be set */

		return -EINVAL;
	}

	if (!(!!info->attrs[GTPA_MS_ADDRESS] ^
	      !!info->attrs[GTPA_MS6_ADDRESS])) {
		/* Either v4 or v6 mobile subscriber address must be set */

		return -EINVAL;
	}

	version = nla_get_u32(info->attrs[GTPA_VERSION]);

	switch (version) {
	case GTP_V0:
		if (!info->attrs[GTPA_TID] ||
		    !info->attrs[GTPA_FLOW])
			return -EINVAL;
		break;
	case GTP_V1:
		if (!info->attrs[GTPA_I_TEI] ||
		    !info->attrs[GTPA_O_TEI])
			return -EINVAL;
		break;

	default:
		return -EINVAL;
	}

	rcu_read_lock();

	gtp = gtp_find_dev(sock_net(skb->sk), info->attrs);
	if (!gtp) {
		err = -ENODEV;
		goto out_unlock;
	}

	if ((info->attrs[GTPA_PEER_ADDRESS] && gtp->is_ipv6) ||
	    (info->attrs[GTPA_PEER6_ADDRESS] && !gtp->is_ipv6)) {
		err = -EINVAL;
		goto out_unlock;
	}

	if (version == GTP_V0)
		sk = gtp->sk0;
	else if (version == GTP_V1)
		sk = gtp->sk1u;
	else
		sk = NULL;

	if (!sk) {
		err = -ENODEV;
		goto out_unlock;
	}

	err = gtp_pdp_add(gtp, sk, info);

out_unlock:
	rcu_read_unlock();
	return err;
}

static struct pdp_ctx *gtp_find_pdp_by_link(struct net *net,
					    struct nlattr *nla[])
{
	struct gtp_dev *gtp;

	gtp = gtp_find_dev(net, nla);
	if (!gtp)
		return ERR_PTR(-ENODEV);

	if (nla[GTPA_MS_ADDRESS]) {
		__be32 ip = nla_get_be32(nla[GTPA_MS_ADDRESS]);

		return ipv4_pdp_find(gtp, ip);
	} else if (nla[GTPA_MS6_ADDRESS]) {
		struct in6_addr ip6 =
		    nla_get_in6_addr(nla[GTPA_MS6_ADDRESS]);

		return ipv6_pdp_find(gtp, &ip6);
	} else if (nla[GTPA_VERSION]) {
		u32 gtp_version = nla_get_u32(nla[GTPA_VERSION]);

		if (gtp_version == GTP_V0 && nla[GTPA_TID])
			return gtp0_pdp_find(gtp, nla_get_u64(nla[GTPA_TID]));
		else if (gtp_version == GTP_V1 && nla[GTPA_I_TEI])
			return gtp1_pdp_find(gtp, nla_get_u32(nla[GTPA_I_TEI]));
	}

	return ERR_PTR(-EINVAL);
}

static struct pdp_ctx *gtp_find_pdp(struct net *net, struct nlattr *nla[])
{
	struct pdp_ctx *pctx;

	if (nla[GTPA_LINK])
		pctx = gtp_find_pdp_by_link(net, nla);
	else
		pctx = ERR_PTR(-EINVAL);

	if (!pctx)
		pctx = ERR_PTR(-ENOENT);

	return pctx;
}

static int gtp_genl_del_pdp(struct sk_buff *skb, struct genl_info *info)
{
	struct pdp_ctx *pctx;
	int err = 0;

	if (!info->attrs[GTPA_VERSION])
		return -EINVAL;

	rcu_read_lock();

	pctx = gtp_find_pdp(sock_net(skb->sk), info->attrs);
	if (IS_ERR(pctx)) {
		err = PTR_ERR(pctx);
		goto out_unlock;
	}

	if (pctx->gtp_version == GTP_V0)
		netdev_dbg(pctx->dev, "GTPv0-U: deleting tunnel id = %llx (pdp %p)\n",
			   pctx->u.v0.tid, pctx);
	else if (pctx->gtp_version == GTP_V1)
		netdev_dbg(pctx->dev, "GTPv1-U: deleting tunnel id = %x/%x (pdp %p)\n",
			   pctx->u.v1.i_tei, pctx->u.v1.o_tei, pctx);

	pdp_context_delete(pctx);

out_unlock:
	rcu_read_unlock();
	return err;
}

static struct genl_family gtp_genl_family;

static int gtp_genl_fill_info(struct sk_buff *skb, u32 snd_portid, u32 snd_seq,
			      u32 type, struct pdp_ctx *pctx)
{
	void *genlh;

	genlh = genlmsg_put(skb, snd_portid, snd_seq, &gtp_genl_family, 0,
			    type);
	if (genlh == NULL)
		goto nlmsg_failure;

	if (nla_put_u32(skb, GTPA_VERSION, pctx->gtp_version))
		goto nla_put_failure;

	if (nla_put_u32(skb, GTPA_LINK, pctx->dev->ifindex))
		goto nla_put_failure;

	switch (pctx->peer_af) {
	case AF_INET:
		if (nla_put_be32(skb, GTPA_PEER_ADDRESS,
				 pctx->peer_addr_ip4.s_addr))
			goto nla_put_failure;

		break;
	case AF_INET6:
		if (nla_put_in6_addr(skb, GTPA_PEER6_ADDRESS,
				     &pctx->peer_addr_ip6))
			goto nla_put_failure;

		break;
	default:
		goto nla_put_failure;
	}

	switch (pctx->ms_af) {
	case AF_INET:
		if (nla_put_be32(skb, GTPA_MS_ADDRESS,
				 pctx->ms_addr_ip4.s_addr))
			goto nla_put_failure;

		break;
	case AF_INET6:
		if (nla_put_in6_addr(skb, GTPA_MS6_ADDRESS,
				     &pctx->ms_addr_ip6))
			goto nla_put_failure;

		break;
	default:
		goto nla_put_failure;
	}

	switch (pctx->gtp_version) {
	case GTP_V0:
		if (nla_put_u64_64bit(skb, GTPA_TID, pctx->u.v0.tid, GTPA_PAD) ||
		    nla_put_u16(skb, GTPA_FLOW, pctx->u.v0.flow))
			goto nla_put_failure;
		break;
	case GTP_V1:
		if (nla_put_u32(skb, GTPA_I_TEI, pctx->u.v1.i_tei) ||
		    nla_put_u32(skb, GTPA_O_TEI, pctx->u.v1.o_tei))
			goto nla_put_failure;
		break;
	}
	genlmsg_end(skb, genlh);
	return 0;

nlmsg_failure:
nla_put_failure:
	genlmsg_cancel(skb, genlh);
	return -EMSGSIZE;
}

static int gtp_genl_get_pdp(struct sk_buff *skb, struct genl_info *info)
{
	struct pdp_ctx *pctx = NULL;
	struct sk_buff *skb2;
	int err;

	if (!info->attrs[GTPA_VERSION])
		return -EINVAL;

	rcu_read_lock();

	pctx = gtp_find_pdp(sock_net(skb->sk), info->attrs);
	if (IS_ERR(pctx)) {
		err = PTR_ERR(pctx);
		goto err_unlock;
	}

	skb2 = genlmsg_new(NLMSG_GOODSIZE, GFP_ATOMIC);
	if (skb2 == NULL) {
		err = -ENOMEM;
		goto err_unlock;
	}

	err = gtp_genl_fill_info(skb2, NETLINK_CB(skb).portid,
				 info->snd_seq, info->nlhdr->nlmsg_type, pctx);
	if (err < 0)
		goto err_unlock_free;

	rcu_read_unlock();
	return genlmsg_unicast(genl_info_net(info), skb2, info->snd_portid);

err_unlock_free:
	kfree_skb(skb2);
err_unlock:
	rcu_read_unlock();
	return err;
}

static int gtp_genl_dump_pdp(struct sk_buff *skb,
				struct netlink_callback *cb)
{
	struct gtp_dev *last_gtp = (struct gtp_dev *)cb->args[2], *gtp;
	struct net *net = sock_net(skb->sk);
	struct gtp_net *gn = net_generic(net, gtp_net_id);
	unsigned long tid = cb->args[1];
	int i, k = cb->args[0], ret;
	struct pdp_ctx *pctx;

	if (cb->args[4])
		return 0;

	list_for_each_entry_rcu(gtp, &gn->gtp_dev_list, list) {
		if (last_gtp && last_gtp != gtp)
			continue;
		else
			last_gtp = NULL;

		for (i = k; i < gtp->hash_size; i++) {
			hlist_for_each_entry_rcu(pctx, &gtp->tid_hash[i], hlist_tid) {
				if (tid && tid != pctx->u.tid)
					continue;
				else
					tid = 0;

				ret = gtp_genl_fill_info(skb,
							 NETLINK_CB(cb->skb).portid,
							 cb->nlh->nlmsg_seq,
							 cb->nlh->nlmsg_type, pctx);
				if (ret < 0) {
					cb->args[0] = i;
					cb->args[1] = pctx->u.tid;
					cb->args[2] = (unsigned long)gtp;
					goto out;
				}
			}
		}
	}
	cb->args[4] = 1;
out:
	return skb->len;
}

static struct nla_policy gtp_genl_policy[GTPA_MAX + 1] = {
	[GTPA_LINK]		= { .type = NLA_U32, },
	[GTPA_VERSION]		= { .type = NLA_U32, },
	[GTPA_TID]		= { .type = NLA_U64, },
	[GTPA_PEER_ADDRESS]	= { .type = NLA_U32, },
	[GTPA_PEER6_ADDRESS]	= { .len = FIELD_SIZEOF(struct ipv6hdr,
							daddr) },
	[GTPA_MS_ADDRESS]	= { .type = NLA_U32, },
	[GTPA_MS6_ADDRESS]	= { .len = FIELD_SIZEOF(struct ipv6hdr,
							daddr) },
	[GTPA_FLOW]		= { .type = NLA_U16, },
	[GTPA_NET_NS_FD]	= { .type = NLA_U32, },
	[GTPA_I_TEI]		= { .type = NLA_U32, },
	[GTPA_O_TEI]		= { .type = NLA_U32, },
};

static const struct genl_ops gtp_genl_ops[] = {
	{
		.cmd = GTP_CMD_NEWPDP,
		.doit = gtp_genl_new_pdp,
		.policy = gtp_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = GTP_CMD_DELPDP,
		.doit = gtp_genl_del_pdp,
		.policy = gtp_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
	{
		.cmd = GTP_CMD_GETPDP,
		.doit = gtp_genl_get_pdp,
		.dumpit = gtp_genl_dump_pdp,
		.policy = gtp_genl_policy,
		.flags = GENL_ADMIN_PERM,
	},
};

static struct genl_family gtp_genl_family __ro_after_init = {
	.name		= GTP_GENL_NAME,
	.version	= GTP_GENL_VERSION,
	.hdrsize	= 0,
	.maxattr	= GTPA_MAX,
	.netnsok	= true,
	.module		= THIS_MODULE,
	.ops		= gtp_genl_ops,
	.n_ops		= ARRAY_SIZE(gtp_genl_ops),
};

static int __net_init gtp_net_init(struct net *net)
{
	struct gtp_net *gn = net_generic(net, gtp_net_id);

	INIT_LIST_HEAD(&gn->gtp_dev_list);
	return 0;
}

static void __net_exit gtp_net_exit(struct net *net)
{
	struct gtp_net *gn = net_generic(net, gtp_net_id);
	struct gtp_dev *gtp;
	LIST_HEAD(list);

	rtnl_lock();
	list_for_each_entry(gtp, &gn->gtp_dev_list, list)
		gtp_dellink(gtp->dev, &list);

	unregister_netdevice_many(&list);
	rtnl_unlock();
}

static struct pernet_operations gtp_net_ops = {
	.init	= gtp_net_init,
	.exit	= gtp_net_exit,
	.id	= &gtp_net_id,
	.size	= sizeof(struct gtp_net),
};

static const struct skb_gso_app gtp_gso_app = {
	.check_flags = SKB_GSO_UDP_TUNNEL | SKB_GSO_UDP_TUNNEL_CSUM,
	.gso_segment = gtp_gso_segment,
};

static int __init gtp_init(void)
{
	int err;

	get_random_bytes(&gtp_h_initval, sizeof(gtp_h_initval));

	err = rtnl_link_register(&gtp_link_ops);
	if (err < 0)
		goto error_out;

	err = genl_register_family(&gtp_genl_family);
	if (err < 0)
		goto unreg_rtnl_link;

	err = register_pernet_subsys(&gtp_net_ops);
	if (err < 0)
		goto unreg_genl_family;

	gtp_gso_type = skb_gso_app_register(&gtp_gso_app);
	if (!gtp_gso_type)
		pr_warn("GTP unable to create UDP app gso type");

	pr_info("GTP module loaded (pdp ctx size %zd bytes)\n",
		sizeof(struct pdp_ctx));
	return 0;

unreg_genl_family:
	genl_unregister_family(&gtp_genl_family);
unreg_rtnl_link:
	rtnl_link_unregister(&gtp_link_ops);
error_out:
	pr_err("error loading GTP module loaded\n");
	return err;
}
late_initcall(gtp_init);

static void __exit gtp_fini(void)
{
	if (gtp_gso_type)
		skb_gso_app_unregister(gtp_gso_type, &gtp_gso_app);

	unregister_pernet_subsys(&gtp_net_ops);
	genl_unregister_family(&gtp_genl_family);
	rtnl_link_unregister(&gtp_link_ops);

	pr_info("GTP module unloaded\n");
}
module_exit(gtp_fini);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Harald Welte <hwelte@sysmocom.de>");
MODULE_DESCRIPTION("Interface driver for GTP encapsulated traffic");
MODULE_ALIAS_RTNL_LINK("gtp");
MODULE_ALIAS_GENL_FAMILY("gtp");
