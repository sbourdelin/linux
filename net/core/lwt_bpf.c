/* Copyright (c) 2016 Thomas Graf <tgraf@tgraf.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/bpf.h>
#include <net/lwtunnel.h>
#include <net/dst_cache.h>
#include <net/ip6_route.h>

struct bpf_lwt_prog {
	struct bpf_prog *prog;
	char *name;
};

struct bpf_lwt {
	struct bpf_lwt_prog in;
	struct bpf_lwt_prog out;
	struct bpf_lwt_prog xmit;
	struct dst_cache dst_cache;
	int family;
};

#define MAX_PROG_NAME 256

static inline struct bpf_lwt *bpf_lwt_lwtunnel(struct lwtunnel_state *lwt)
{
	return (struct bpf_lwt *)lwt->data;
}

#define NO_REDIRECT false
#define CAN_REDIRECT true

static int run_lwt_bpf(struct sk_buff *skb, struct bpf_lwt_prog *lwt,
		       struct dst_entry *dst, bool can_redirect)
{
	int ret;

	/* Preempt disable is needed to protect per-cpu redirect_info between
	 * BPF prog and skb_do_redirect(). The call_rcu in bpf_prog_put() and
	 * access to maps strictly require a rcu_read_lock() for protection,
	 * mixing with BH RCU lock doesn't work.
	 */
	preempt_disable();
	rcu_read_lock();
	bpf_compute_data_end(skb);
	ret = BPF_PROG_RUN(lwt->prog, skb);
	rcu_read_unlock();

	switch (ret) {
	case BPF_OK:
		break;

	case BPF_REDIRECT:
		if (!can_redirect) {
			WARN_ONCE(1, "Illegal redirect return code in prog %s\n",
				  lwt->name ? : "<unknown>");
			ret = BPF_OK;
		} else {
			ret = skb_do_redirect(skb);
			if (ret == 0)
				ret = BPF_REDIRECT;
		}
		break;

	case BPF_DROP:
		kfree_skb(skb);
		ret = -EPERM;
		break;

	case BPF_LWT_REROUTE:
		break;

	default:
		WARN_ONCE(1, "Illegal LWT BPF return value %u, expect packet loss\n",
			  ret);
		kfree_skb(skb);
		ret = -EINVAL;
		break;
	}

	preempt_enable();

	return ret;
}

static int bpf_input(struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);
	struct bpf_lwt *bpf;
	int ret;

	bpf = bpf_lwt_lwtunnel(dst->lwtstate);
	if (bpf->in.prog) {
		ret = run_lwt_bpf(skb, &bpf->in, dst, NO_REDIRECT);
		if (ret < 0)
			return ret;
	}

	if (unlikely(!dst->lwtstate->orig_input)) {
		WARN_ONCE(1, "orig_input not set on dst for prog %s\n",
			  bpf->out.name);
		kfree_skb(skb);
		return -EINVAL;
	}

	return dst->lwtstate->orig_input(skb);
}

#if IS_ENABLED(CONFIG_IPV6)
static struct dst_entry *bpf_lwt_lookup6(struct net *net, struct sk_buff *skb,
					 struct bpf_lwt *bpf)
{
	struct ipv6hdr *ip6h = ipv6_hdr(skb);
	struct dst_entry *dst;
	struct flowi6 fl6 = {
		.daddr = ip6h->daddr,
		.saddr = ip6h->saddr,
		.flowlabel = ip6_flowinfo(ip6h),
		.flowi6_mark = skb->mark,
		.flowi6_proto = ip6h->nexthdr,
		.flowi6_oif = skb->sk ? skb->sk->sk_bound_dev_if : 0,
	};

	dst = ip6_route_output(net, skb->sk, &fl6);
	if (unlikely(dst->error)) {
		int err = dst->error;
		dst_release(dst);
		return ERR_PTR(err);
	}

	dst = xfrm_lookup(net, dst, flowi6_to_flowi(&fl6), NULL, 0);
	if (IS_ERR(dst))
		return dst;

	dst_cache_set_ip6(&bpf->dst_cache, dst, &fl6.saddr);

	return dst;
}
#endif

static struct dst_entry *bpf_lwt_lookup4(struct net *net, struct sk_buff *skb,
					 struct bpf_lwt *bpf)
{
	struct iphdr *ip4 = ip_hdr(skb);
	struct dst_entry *dst;
	struct rtable *rt;
	struct flowi4 fl4 = {
		.flowi4_oif = skb->sk ? skb->sk->sk_bound_dev_if : 0,
		.flowi4_mark = skb->mark,
		.flowi4_proto = ip4->protocol,
		.flowi4_tos = RT_TOS(ip4->tos),
		.flowi4_flags = skb->sk ? inet_sk_flowi_flags(skb->sk) : 0,
		.saddr = ip4->saddr,
		.daddr = ip4->daddr,
	};

	rt = ip_route_output_key(net, &fl4);
	if (IS_ERR(rt))
		return ERR_CAST(rt);

	dst = &rt->dst;
	if (dst->error) {
		int err = dst->error;
		dst_release(dst);
		return ERR_PTR(err);
	}

	dst = xfrm_lookup(net, dst, flowi4_to_flowi(&fl4), NULL, 0);
	if (IS_ERR(dst))
		return dst;

	dst_cache_set_ip4(&bpf->dst_cache, dst, fl4.saddr);

	return dst;
}

static int bpf_lwt_reroute(struct net *net, struct sk_buff *skb,
			   struct bpf_lwt *bpf)
{
	struct dst_entry *dst;

	dst = dst_cache_get(&bpf->dst_cache);
	if (unlikely(!dst)) {
		switch (bpf->family) {
		case AF_INET:
			dst = bpf_lwt_lookup4(net, skb, bpf);
			break;
#if IS_ENABLED(CONFIG_IPV6)
		case AF_INET6:
			dst = bpf_lwt_lookup6(net, skb, bpf);
			break;
#endif
		default:
			return -EAFNOSUPPORT;
		}

		if (IS_ERR(dst))
			return PTR_ERR(dst);
	}

	skb_dst_drop(skb);
	skb_dst_set(skb, dst);

	return 0;
}

static int bpf_output(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);
	struct bpf_lwt *bpf;
	int ret;

	bpf = bpf_lwt_lwtunnel(dst->lwtstate);
	if (bpf->out.prog) {
		ret = run_lwt_bpf(skb, &bpf->out, dst, NO_REDIRECT);
		if (ret < 0)
			return ret;

		if (ret == BPF_LWT_REROUTE) {
			ret = bpf_lwt_reroute(net, skb, bpf);
			if (ret < 0) {
				kfree_skb(skb);
				return ret;
			}

			return dst_output(net, sk, skb);
		}
	}

	if (unlikely(!dst->lwtstate->orig_output)) {
		WARN_ONCE(1, "orig_output not set on dst for prog %s\n",
			  bpf->out.name);
		kfree_skb(skb);
		return -EINVAL;
	}

	return dst->lwtstate->orig_output(net, sk, skb);
}

static int xmit_check_hhlen(struct sk_buff *skb)
{
	int hh_len = skb_dst(skb)->dev->hard_header_len;

	if (skb_headroom(skb) < hh_len) {
		int nhead = HH_DATA_ALIGN(hh_len - skb_headroom(skb));

		if (pskb_expand_head(skb, nhead, 0, GFP_ATOMIC))
			return -ENOMEM;
	}

	return 0;
}

static int bpf_xmit(struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);
	struct bpf_lwt *bpf;

	bpf = bpf_lwt_lwtunnel(dst->lwtstate);
	if (bpf->xmit.prog) {
		int ret;

		ret = run_lwt_bpf(skb, &bpf->xmit, dst, CAN_REDIRECT);
		switch (ret) {
		case BPF_OK:
			/* If the L3 header was expanded, headroom might be too
			 * small for L2 header now, expand as needed.
			 */
			ret = xmit_check_hhlen(skb);
			if (unlikely(ret))
				return ret;

			return LWTUNNEL_XMIT_CONTINUE;
		case BPF_REDIRECT:
			return LWTUNNEL_XMIT_DONE;
		default:
			return ret;
		}
	}

	return LWTUNNEL_XMIT_CONTINUE;
}

static void bpf_lwt_prog_destroy(struct bpf_lwt_prog *prog)
{
	if (prog->prog)
		bpf_prog_put(prog->prog);

	kfree(prog->name);
}

static void bpf_destroy_state(struct lwtunnel_state *lwt)
{
	struct bpf_lwt *bpf = bpf_lwt_lwtunnel(lwt);

	dst_cache_destroy(&bpf->dst_cache);
	bpf_lwt_prog_destroy(&bpf->in);
	bpf_lwt_prog_destroy(&bpf->out);
	bpf_lwt_prog_destroy(&bpf->xmit);
}

static const struct nla_policy bpf_prog_policy[LWT_BPF_PROG_MAX + 1] = {
	[LWT_BPF_PROG_FD] = { .type = NLA_U32, },
	[LWT_BPF_PROG_NAME] = { .type = NLA_NUL_STRING,
				.len = MAX_PROG_NAME },
};

static int bpf_parse_prog(struct nlattr *attr, struct bpf_lwt_prog *prog,
			  enum bpf_prog_type type)
{
	struct nlattr *tb[LWT_BPF_PROG_MAX + 1];
	struct bpf_prog *p;
	int ret;
	u32 fd;

	ret = nla_parse_nested(tb, LWT_BPF_PROG_MAX, attr, bpf_prog_policy);
	if (ret < 0)
		return ret;

	if (!tb[LWT_BPF_PROG_FD] || !tb[LWT_BPF_PROG_NAME])
		return -EINVAL;

	prog->name = nla_memdup(tb[LWT_BPF_PROG_NAME], GFP_KERNEL);
	if (!prog->name)
		return -ENOMEM;

	fd = nla_get_u32(tb[LWT_BPF_PROG_FD]);
	p = bpf_prog_get_type(fd, type);
	if (IS_ERR(p))
		return PTR_ERR(p);

	prog->prog = p;

	return 0;
}

static const struct nla_policy bpf_nl_policy[LWT_BPF_MAX + 1] = {
	[LWT_BPF_IN]   = { .type = NLA_NESTED, },
	[LWT_BPF_OUT]  = { .type = NLA_NESTED, },
	[LWT_BPF_XMIT] = { .type = NLA_NESTED, },
};

static int bpf_build_state(struct net_device *dev, struct nlattr *nla,
			   unsigned int family, const void *cfg,
			   struct lwtunnel_state **ts)
{
	struct nlattr *tb[LWT_BPF_MAX + 1];
	struct lwtunnel_state *newts;
	struct bpf_lwt *bpf;
	int ret;

	if (family != AF_INET && family != AF_INET6)
		return -EAFNOSUPPORT;

	ret = nla_parse_nested(tb, LWT_BPF_MAX, nla, bpf_nl_policy);
	if (ret < 0)
		return ret;

	if (!tb[LWT_BPF_IN] && !tb[LWT_BPF_OUT] && !tb[LWT_BPF_XMIT])
		return -EINVAL;

	newts = lwtunnel_state_alloc(sizeof(*bpf));
	if (!newts)
		return -ENOMEM;

	newts->type = LWTUNNEL_ENCAP_BPF;
	bpf = bpf_lwt_lwtunnel(newts);

	if (tb[LWT_BPF_IN]) {
		newts->flags |= LWTUNNEL_STATE_INPUT_REDIRECT;
		ret = bpf_parse_prog(tb[LWT_BPF_IN], &bpf->in,
				     BPF_PROG_TYPE_LWT_IN);
		if (ret  < 0)
			goto errout;
	}

	if (tb[LWT_BPF_OUT]) {
		newts->flags |= LWTUNNEL_STATE_OUTPUT_REDIRECT;
		ret = bpf_parse_prog(tb[LWT_BPF_OUT], &bpf->out,
				     BPF_PROG_TYPE_LWT_OUT);
		if (ret < 0)
			goto errout;
	}

	if (tb[LWT_BPF_XMIT]) {
		newts->flags |= LWTUNNEL_STATE_XMIT_REDIRECT;
		ret = bpf_parse_prog(tb[LWT_BPF_XMIT], &bpf->xmit,
				     BPF_PROG_TYPE_LWT_XMIT);
		if (ret < 0)
			goto errout;
	}

	ret = dst_cache_init(&bpf->dst_cache, GFP_KERNEL);
	if (ret)
		goto errout;

	bpf->family = family;
	*ts = newts;

	return 0;

errout:
	bpf_destroy_state(newts);
	kfree(newts);
	return ret;
}

static int bpf_fill_lwt_prog(struct sk_buff *skb, int attr,
			     struct bpf_lwt_prog *prog)
{
	struct nlattr *nest;

	if (!prog->prog)
		return 0;

	nest = nla_nest_start(skb, attr);
	if (!nest)
		return -EMSGSIZE;

	if (prog->name &&
	    nla_put_string(skb, LWT_BPF_PROG_NAME, prog->name))
		return -EMSGSIZE;

	return nla_nest_end(skb, nest);
}

static int bpf_fill_encap_info(struct sk_buff *skb, struct lwtunnel_state *lwt)
{
	struct bpf_lwt *bpf = bpf_lwt_lwtunnel(lwt);

	if (bpf_fill_lwt_prog(skb, LWT_BPF_IN, &bpf->in) < 0 ||
	    bpf_fill_lwt_prog(skb, LWT_BPF_OUT, &bpf->out) < 0 ||
	    bpf_fill_lwt_prog(skb, LWT_BPF_XMIT, &bpf->xmit) < 0)
		return -EMSGSIZE;

	return 0;
}

static int bpf_encap_nlsize(struct lwtunnel_state *lwtstate)
{
	int nest_len = nla_total_size(sizeof(struct nlattr)) +
		       nla_total_size(MAX_PROG_NAME) + /* LWT_BPF_PROG_NAME */
		       0;

	return nest_len + /* LWT_BPF_IN */
	       nest_len + /* LWT_BPF_OUT */
	       nest_len + /* LWT_BPF_XMIT */
	       0;
}

int bpf_lwt_prog_cmp(struct bpf_lwt_prog *a, struct bpf_lwt_prog *b)
{
	/* FIXME:
	 * The LWT state is currently rebuilt for delete requests which
	 * results in a new bpf_prog instance. Comparing names for now.
	 */
	if (!a->name && !b->name)
		return 0;

	if (!a->name || !b->name)
		return 1;

	return strcmp(a->name, b->name);
}

static int bpf_encap_cmp(struct lwtunnel_state *a, struct lwtunnel_state *b)
{
	struct bpf_lwt *a_bpf = bpf_lwt_lwtunnel(a);
	struct bpf_lwt *b_bpf = bpf_lwt_lwtunnel(b);

	return bpf_lwt_prog_cmp(&a_bpf->in, &b_bpf->in) ||
	       bpf_lwt_prog_cmp(&a_bpf->out, &b_bpf->out) ||
	       bpf_lwt_prog_cmp(&a_bpf->xmit, &b_bpf->xmit);
}

static const struct lwtunnel_encap_ops bpf_encap_ops = {
	.build_state	= bpf_build_state,
	.destroy_state	= bpf_destroy_state,
	.input		= bpf_input,
	.output		= bpf_output,
	.xmit		= bpf_xmit,
	.fill_encap	= bpf_fill_encap_info,
	.get_encap_size = bpf_encap_nlsize,
	.cmp_encap	= bpf_encap_cmp,
};

static int __init bpf_lwt_init(void)
{
	return lwtunnel_encap_add_ops(&bpf_encap_ops, LWTUNNEL_ENCAP_BPF);
}

subsys_initcall(bpf_lwt_init)
