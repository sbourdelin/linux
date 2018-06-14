#include <linux/kernel.h>
#include <linux/netfilter.h>
#include <linux/types.h>
#include <net/xfrm.h>
#include <net/arp.h>
#include <net/udp.h>
#include <net/tcp.h>
#include <net/esp.h>
#include <net/protocol.h>
#include <crypto/aead.h>
#include <net/netfilter/early_ingress.h>

/* XXX: Maybe export this from net/core/skbuff.c
 * instead of holding a local copy */
static void skb_headers_offset_update(struct sk_buff *skb, int off)
{
	/* Only adjust this if it actually is csum_start rather than csum */
	if (skb->ip_summed == CHECKSUM_PARTIAL)
		skb->csum_start += off;
	/* {transport,network,mac}_header and tail are relative to skb->head */
	skb->transport_header += off;
	skb->network_header   += off;
	if (skb_mac_header_was_set(skb))
		skb->mac_header += off;
	skb->inner_transport_header += off;
	skb->inner_network_header += off;
	skb->inner_mac_header += off;
}

struct sk_buff *nft_skb_segment(struct sk_buff *head_skb)
{
	unsigned int headroom;
	struct sk_buff *nskb;
	struct sk_buff *segs = NULL;
	struct sk_buff *tail = NULL;
	unsigned int doffset = head_skb->data - skb_mac_header(head_skb);
	struct sk_buff *list_skb = skb_shinfo(head_skb)->frag_list;
	unsigned int tnl_hlen = skb_tnl_header_len(head_skb);
	unsigned int delta_segs, delta_len, delta_truesize;

	__skb_push(head_skb, doffset);

	headroom = skb_headroom(head_skb);

	delta_segs = delta_len = delta_truesize = 0;

	skb_shinfo(head_skb)->frag_list = NULL;

	segs = skb_clone(head_skb, GFP_ATOMIC);
	if (unlikely(!segs))
		return ERR_PTR(-ENOMEM);

	do {
		nskb = list_skb;

		list_skb = list_skb->next;

		if (!tail)
			segs->next = nskb;
		else
			tail->next = nskb;

		tail = nskb;

		delta_len += nskb->len;
		delta_truesize += nskb->truesize;

		skb_push(nskb, doffset);

		nskb->dev = head_skb->dev;
		nskb->queue_mapping = head_skb->queue_mapping;
		nskb->network_header = head_skb->network_header;
		nskb->mac_len = head_skb->mac_len;
		nskb->mac_header = head_skb->mac_header;
		nskb->transport_header = head_skb->transport_header;

		if (!secpath_exists(nskb))
			nskb->sp = secpath_get(head_skb->sp);

		skb_headers_offset_update(nskb, skb_headroom(nskb) - headroom);

		skb_copy_from_linear_data_offset(head_skb, -tnl_hlen,
						 nskb->data - tnl_hlen,
						 doffset + tnl_hlen);

	} while (list_skb);

	segs->len = head_skb->len - delta_len;
	segs->data_len = head_skb->data_len - delta_len;
	segs->truesize += head_skb->data_len - delta_truesize;

	head_skb->len = segs->len;
	head_skb->data_len = segs->data_len;
	head_skb->truesize += segs->truesize;

	skb_shinfo(segs)->gso_size = 0;
	skb_shinfo(segs)->gso_segs = 0;
	skb_shinfo(segs)->gso_type = 0;

	segs->prev = tail;

	return segs;
}

static int nft_skb_gro_receive(struct sk_buff **head, struct sk_buff *skb)
{
	struct sk_buff *p = *head;

	if (unlikely((!NAPI_GRO_CB(p)->is_ffwd) || !skb_dst(p)))
		return -EINVAL;

	if (NAPI_GRO_CB(p)->last == p)
		skb_shinfo(p)->frag_list = skb;
	else
		NAPI_GRO_CB(p)->last->next = skb;
	NAPI_GRO_CB(p)->last = skb;

	NAPI_GRO_CB(p)->count++;
	p->data_len += skb->len;
	p->truesize += skb->truesize;
	p->len += skb->len;

	NAPI_GRO_CB(skb)->same_flow = 1;
	return 0;
}

static struct sk_buff **udp_gro_ffwd_receive(struct sk_buff **head,
					     struct sk_buff *skb,
					     struct udphdr *uh)
{
	struct sk_buff *p = NULL;
	struct sk_buff **pp = NULL;
	struct udphdr *uh2;
	int flush = 0;

	for (; (p = *head); head = &p->next) {

		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		uh2 = udp_hdr(p);

		/* Match ports and either checksums are either both zero
		 * or nonzero.
		 */
		if ((*(u32 *)&uh->source != *(u32 *)&uh2->source) ||
		    (!uh->check ^ !uh2->check)) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}

		goto found;
	}

	goto out;

found:
	p = *head;

	if (nft_skb_gro_receive(head, skb))
		flush = 1;

out:
	if (p && (!NAPI_GRO_CB(skb)->same_flow || flush))
		pp = head;

	NAPI_GRO_CB(skb)->flush |= flush;
	return pp;
}

struct sk_buff **nft_udp_gro_receive(struct sk_buff **head, struct sk_buff *skb)
{
	struct udphdr *uh;

	uh = skb_gro_header_slow(skb, skb_transport_offset(skb) + sizeof(struct udphdr),
				 skb_transport_offset(skb));

	if (unlikely(!uh))
		goto flush;

	if (NAPI_GRO_CB(skb)->flush)
		goto flush;

	if (NAPI_GRO_CB(skb)->is_ffwd)
		return udp_gro_ffwd_receive(head, skb, uh);

flush:
	NAPI_GRO_CB(skb)->flush = 1;
	return NULL;
}

struct sk_buff **nft_tcp_gro_receive(struct sk_buff **head, struct sk_buff *skb)
{
	struct sk_buff **pp = NULL;
	struct sk_buff *p;
	struct tcphdr *th;
	struct tcphdr *th2;
	unsigned int len;
	unsigned int thlen;
	__be32 flags;
	unsigned int mss = 1;
	unsigned int hlen;
	int flush = 1;
	int i;

	th = skb_gro_header_slow(skb, skb_transport_offset(skb) + sizeof(struct tcphdr),
				 skb_transport_offset(skb));
	if (unlikely(!th))
		goto out;

	thlen = th->doff * 4;
	if (thlen < sizeof(*th))
		goto out;

	hlen = skb_transport_offset(skb) + thlen;

	th = skb_gro_header_slow(skb, hlen, skb_transport_offset(skb));
	if (unlikely(!th))
		goto out;

	skb_gro_pull(skb, thlen);
	len = skb_gro_len(skb);
	flags = tcp_flag_word(th);

	for (; (p = *head); head = &p->next) {
		if (!NAPI_GRO_CB(p)->same_flow)
			continue;

		th2 = tcp_hdr(p);

		if (*(u32 *)&th->source ^ *(u32 *)&th2->source) {
			NAPI_GRO_CB(p)->same_flow = 0;
			continue;
		}

		goto found;
	}

	goto out_check_final;

found:
	flush = NAPI_GRO_CB(p)->flush;
	flush |= (__force int)(flags & TCP_FLAG_CWR);
	flush |= (__force int)((flags ^ tcp_flag_word(th2)) &
		  ~(TCP_FLAG_CWR | TCP_FLAG_FIN | TCP_FLAG_PSH));
	flush |= (__force int)(th->ack_seq ^ th2->ack_seq);
	for (i = sizeof(*th); i < thlen; i += 4)
		flush |= *(u32 *)((u8 *)th + i) ^
			 *(u32 *)((u8 *)th2 + i);

	mss = skb_shinfo(p)->gso_size;

	flush |= (len - 1) >= mss;
	flush |= (ntohl(th2->seq) + (skb_gro_len(p) - (hlen * (NAPI_GRO_CB(p)->count - 1)))) ^ ntohl(th->seq);

	if (flush || nft_skb_gro_receive(head, skb)) {
		mss = 1;
		goto out_check_final;
	}

	p = *head;

out_check_final:
	flush = len < mss;
	flush |= (__force int)(flags & (TCP_FLAG_URG | TCP_FLAG_PSH |
					TCP_FLAG_RST | TCP_FLAG_SYN |
					TCP_FLAG_FIN));

	if (p && (!NAPI_GRO_CB(skb)->same_flow || flush))
		pp = head;

out:
	NAPI_GRO_CB(skb)->flush |= (flush != 0);

	return pp;
}

struct sk_buff *nft_esp_gso_segment(struct sk_buff *skb,
				    netdev_features_t features)
{
	struct xfrm_offload *xo = xfrm_offload(skb);
	netdev_features_t esp_features = features;
	struct crypto_aead *aead;
	struct ip_esp_hdr *esph;
	struct xfrm_state *x;

	if (!xo)
		return ERR_PTR(-EINVAL);

	x = skb->sp->xvec[skb->sp->len - 1];
	aead = x->data;
	esph = ip_esp_hdr(skb);

	if (esph->spi != x->id.spi)
		return ERR_PTR(-EINVAL);

	if (!pskb_may_pull(skb, sizeof(*esph) + crypto_aead_ivsize(aead)))
		return ERR_PTR(-EINVAL);

	__skb_pull(skb, sizeof(*esph) + crypto_aead_ivsize(aead));

	skb->encap_hdr_csum = 1;

	if (!(features & NETIF_F_HW_ESP) || !x->xso.offload_handle ||
	    (x->xso.dev != skb->dev))
		esp_features = features & ~(NETIF_F_SG | NETIF_F_CSUM_MASK);

	xo->flags |= XFRM_GSO_SEGMENT;

	return x->outer_mode->gso_segment(x, skb, esp_features);
}

static inline bool nf_hook_early_ingress_active(const struct sk_buff *skb)
{
#ifdef HAVE_JUMP_LABEL
	if (!static_key_false(&nf_hooks_needed[NFPROTO_NETDEV][NF_NETDEV_EARLY_INGRESS]))
		return false;
#endif
	return rcu_access_pointer(skb->dev->nf_hooks_early_ingress);
}

int nf_hook_early_ingress(struct sk_buff *skb)
{
	struct nf_hook_entries *e =
		rcu_dereference(skb->dev->nf_hooks_early_ingress);
	struct nf_hook_state state;
	int ret = NF_ACCEPT;

	if (nf_hook_early_ingress_active(skb)) {
		if (unlikely(!e))
			return 0;

		nf_hook_state_init(&state, NF_NETDEV_EARLY_INGRESS,
				   NFPROTO_NETDEV, skb->dev, NULL, NULL,
				   dev_net(skb->dev), NULL);

		ret = nf_hook_netdev(skb, &state, e);
	}

	return ret;
}

/* protected by nf_hook_mutex. */
static int nf_early_ingress_use;

void nf_early_ingress_enable(void)
{
	if (nf_early_ingress_use++ == 0) {
		nf_early_ingress_use++;
		nf_early_ingress_ip_enable();
		nf_early_ingress_ip6_enable();
	}
}

void nf_early_ingress_disable(void)
{
	if (--nf_early_ingress_use == 0) {
		nf_early_ingress_ip_disable();
		nf_early_ingress_ip6_disable();
	}
}
