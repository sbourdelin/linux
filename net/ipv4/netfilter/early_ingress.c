#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/types.h>
#include <net/xfrm.h>
#include <net/arp.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/protocol.h>
#include <net/netfilter/early_ingress.h>

static const struct net_offload __rcu *nft_ip_offloads[MAX_INET_PROTOS] __read_mostly;

static struct sk_buff *nft_udp4_gso_segment(struct sk_buff *skb,
					    netdev_features_t features)
{
	skb_push(skb, sizeof(struct iphdr));
	return nft_skb_segment(skb);
}

static struct sk_buff *nft_tcp4_gso_segment(struct sk_buff *skb,
					    netdev_features_t features)
{
	skb_push(skb, sizeof(struct iphdr));
	return nft_skb_segment(skb);
}

static struct sk_buff *nft_ipv4_gso_segment(struct sk_buff *skb,
					    netdev_features_t features)
{
	struct sk_buff *segs = ERR_PTR(-EINVAL);
	const struct net_offload *ops;
	struct packet_offload *ptype;
	struct iphdr *iph;
	int proto;
	int ihl;

	if (!(skb_shinfo(skb)->gso_type & SKB_GSO_NFT)) {
		ptype = dev_get_packet_offload(skb->protocol, 1);
		if (ptype)
			return ptype->callbacks.gso_segment(skb, features);

		return ERR_PTR(-EPROTONOSUPPORT);
	}

	if (SKB_GSO_CB(skb)->encap_level == 0) {
		iph = ip_hdr(skb);
		skb_reset_network_header(skb);
	} else {
		iph = (struct iphdr *)skb->data;
	}

	if (unlikely(!pskb_may_pull(skb, sizeof(*iph))))
		goto out;

	ihl = iph->ihl * 4;
	if (ihl < sizeof(*iph))
		goto out;

	SKB_GSO_CB(skb)->encap_level += ihl;

	if (unlikely(!pskb_may_pull(skb, ihl)))
		goto out;

	__skb_pull(skb, ihl);

	proto = iph->protocol;

	segs = ERR_PTR(-EPROTONOSUPPORT);

	ops = rcu_dereference(nft_ip_offloads[proto]);
	if (likely(ops && ops->callbacks.gso_segment))
		segs = ops->callbacks.gso_segment(skb, features);

out:
	return segs;
}

static int nft_ipv4_gro_complete(struct sk_buff *skb, int nhoff)
{
	struct iphdr *iph = (struct iphdr *)(skb->data + nhoff);
	struct dst_entry *dst = skb_dst(skb);
	struct rtable *rt = (struct rtable *)dst;
	const struct net_offload *ops;
	struct packet_offload *ptype;
	struct net_device *dev;
	struct neighbour *neigh;
	unsigned int hh_len;
	int err = 0;
	u32 nexthop;
	u16 count;

	count = NAPI_GRO_CB(skb)->count;

	if (!NAPI_GRO_CB(skb)->is_ffwd) {
		ptype = dev_get_packet_offload(skb->protocol, 1);
		if (ptype)
			return ptype->callbacks.gro_complete(skb, nhoff);

		return 0;
	}

	rcu_read_lock();
	ops = rcu_dereference(nft_ip_offloads[iph->protocol]);
	if (!ops || !ops->callbacks.gro_complete)
		goto out_unlock;

	/* Only need to add sizeof(*iph) to get to the next hdr below
	 * because any hdr with option will have been flushed in
	 * inet_gro_receive().
	 */
	err = ops->callbacks.gro_complete(skb, nhoff + sizeof(*iph));

out_unlock:
	rcu_read_unlock();

	if (err)
		return err;

	skb_shinfo(skb)->gso_type |= SKB_GSO_NFT;
	skb_shinfo(skb)->gso_segs = count;

	dev = dst->dev;
	dev_hold(dev);
	skb->dev = dev;

	if (skb_dst(skb)->xfrm) {
		err = dst_output(dev_net(dev), NULL, skb);
		if (err != -EREMOTE)
			return -EINPROGRESS;
	}

	if (count <= 1)
		skb_gso_reset(skb);

	hh_len = LL_RESERVED_SPACE(dev);

	if (unlikely(skb_headroom(skb) < hh_len && dev->header_ops)) {
		struct sk_buff *skb2;

		skb2 = skb_realloc_headroom(skb, LL_RESERVED_SPACE(dev));
		if (!skb2) {
			kfree_skb(skb);
			return -ENOMEM;
		}
		consume_skb(skb);
		skb = skb2;
	}
	rcu_read_lock();
	nexthop = (__force u32) rt_nexthop(rt, iph->daddr);
	neigh = __ipv4_neigh_lookup_noref(dev, nexthop);
	if (unlikely(!neigh))
		neigh = __neigh_create(&arp_tbl, &nexthop, dev, false);
	if (!IS_ERR(neigh))
		neigh_output(neigh, skb);
	rcu_read_unlock();

	return -EINPROGRESS;
}

static struct sk_buff **nft_ipv4_gro_receive(struct sk_buff **head,
					     struct sk_buff *skb)
{
	const struct net_offload *ops;
	struct packet_offload *ptype;
	struct sk_buff **pp = NULL;
	struct sk_buff *p;
	struct iphdr *iph;
	unsigned int hlen;
	unsigned int off;
	int proto, ret;

	off = skb_gro_offset(skb);
	hlen = off + sizeof(*iph);

	iph = skb_gro_header_slow(skb, hlen, off);
	if (unlikely(!iph)) {
		pp = ERR_PTR(-EPERM);
		goto out;
	}

	proto = iph->protocol;

	rcu_read_lock();

	if (*(u8 *)iph != 0x45) {
		kfree_skb(skb);
		pp = ERR_PTR(-EPERM);
		goto out_unlock;
	}

	if (unlikely(ip_fast_csum((u8 *)iph, 5))) {
		kfree_skb(skb);
		pp = ERR_PTR(-EPERM);
		goto out_unlock;
	}

	if (ip_is_fragment(iph))
		goto out_unlock;

	ret = nf_hook_early_ingress(skb);
	switch (ret) {
	case NF_STOLEN:
		break;
	case NF_ACCEPT:
		ptype = dev_get_packet_offload(skb->protocol, 1);
		if (ptype)
			pp = ptype->callbacks.gro_receive(head, skb);

		goto out_unlock;
	case NF_DROP:
		pp = ERR_PTR(-EPERM);
		goto out_unlock;
	}

	ops = rcu_dereference(nft_ip_offloads[proto]);
	if (!ops || !ops->callbacks.gro_receive)
		goto out_unlock;

	if (iph->ttl <= 1) {
		kfree_skb(skb);
		pp = ERR_PTR(-EPERM);
		goto out_unlock;
	}

	skb->ip_summed = CHECKSUM_UNNECESSARY;

	for (p = *head; p; p = p->next) {
		struct iphdr *iph2;

		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		iph2 = ip_hdr(p);
		/* The above works because, with the exception of the top
		 * (inner most) layer, we only aggregate pkts with the same
		 * hdr length so all the hdrs we'll need to verify will start
		 * at the same offset.
		 */
		if ((iph->protocol ^ iph2->protocol) |
		    ((__force u32)iph->saddr ^ (__force u32)iph2->saddr) |
		    ((__force u32)iph->daddr ^ (__force u32)iph2->daddr)) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}

		if (!NAPI_GRO_CB(p)->is_ffwd)
			continue;

		if (!skb_dst(p))
			continue;

		/* All fields must match except length and checksum. */
		NAPI_GRO_CB(p)->flush |=
			((iph->ttl - 1) ^ iph2->ttl) |
			(iph->tos ^ iph2->tos) |
			((iph->frag_off ^ iph2->frag_off) & htons(IP_DF));

		pp = &p;

		break;
	}

	NAPI_GRO_CB(skb)->is_atomic = !!(iph->frag_off & htons(IP_DF));

	ip_decrease_ttl(iph);
	skb->priority = rt_tos2priority(iph->tos);

	skb_pull(skb, off);
	NAPI_GRO_CB(skb)->data_offset = sizeof(*iph);
	skb_reset_network_header(skb);
	skb_set_transport_header(skb, sizeof(*iph));

	pp = call_gro_receive(ops->callbacks.gro_receive, head, skb);
out_unlock:
	rcu_read_unlock();

out:
	NAPI_GRO_CB(skb)->data_offset = 0;
	return pp;
}

static struct packet_offload nft_ipv4_packet_offload __read_mostly = {
	.type = cpu_to_be16(ETH_P_IP),
	.priority = 0,
	.callbacks = {
		.gro_receive = nft_ipv4_gro_receive,
		.gro_complete = nft_ipv4_gro_complete,
		.gso_segment = nft_ipv4_gso_segment,
	},
};

static const struct net_offload nft_udp4_offload = {
	.callbacks = {
		.gso_segment = nft_udp4_gso_segment,
		.gro_receive  =	nft_udp_gro_receive,
	},
};

static const struct net_offload nft_tcp4_offload = {
	.callbacks = {
		.gso_segment = nft_tcp4_gso_segment,
		.gro_receive  =	nft_tcp_gro_receive,
	},
};

static const struct net_offload __rcu *nft_ip_offloads[MAX_INET_PROTOS] __read_mostly = {
	[IPPROTO_UDP]	= &nft_udp4_offload,
	[IPPROTO_TCP]	= &nft_tcp4_offload,
};

void nf_early_ingress_ip_enable(void)
{
	dev_add_offload(&nft_ipv4_packet_offload);
}

void nf_early_ingress_ip_disable(void)
{
	dev_remove_offload(&nft_ipv4_packet_offload);
}
