// SPDX-License-Identifier: GPL-2.0
#include <linux/errno.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/types.h>
#include <net/checksum.h>
#include <net/dst_cache.h>
#include <net/ip.h>
#include <net/ip6_fib.h>
#include <net/ip6_route.h>
#include <net/lwtunnel.h>
#include <net/protocol.h>
#include <uapi/linux/ila.h>
#include "ila.h"

struct ila_lwt {
	struct ila_params p;
	struct dst_cache dst_cache;
	u8 hook_type;
	u32 connected : 1;
	u32 xlat : 1;
	u32 notify : 2;
};

#define ILA_NOTIFY_DST 1
#define ILA_NOTIFY_SRC 2

static inline struct ila_lwt *ila_lwt_lwtunnel(
	struct lwtunnel_state *lwt)
{
	return (struct ila_lwt *)lwt->data;
}

static inline struct ila_params *ila_params_lwtunnel(
	struct lwtunnel_state *lwt)
{
	return &ila_lwt_lwtunnel(lwt)->p;
}

static size_t ila_rslv_msgsize(void)
{
	size_t len =
		NLMSG_ALIGN(sizeof(struct rtmsg))
		+ nla_total_size(16)     /* RTA_DST */
		+ nla_total_size(16)     /* RTA_SRC */
		;

	return len;
}

void ila_notify(struct net *net, struct sk_buff *skb, struct ila_lwt *lwt)
{
	struct ipv6hdr *ip6h = ipv6_hdr(skb);
	int flags = NLM_F_MULTI;
	struct sk_buff *nlskb;
	struct nlmsghdr *nlh;
	struct rtmsg *rtm;
	int err = 0;

	/* Send ILA notification to user */
	nlskb = nlmsg_new(ila_rslv_msgsize(), GFP_KERNEL);
	if (!nlskb)
		return;

	nlh = nlmsg_put(nlskb, 0, 0, RTM_ADDR_RESOLVE, sizeof(*rtm), flags);
	if (!nlh) {
		err = -EMSGSIZE;
		goto errout;
	}

	rtm = nlmsg_data(nlh);
	rtm->rtm_family   = AF_INET6;
	rtm->rtm_dst_len  = 128;
	rtm->rtm_src_len  = 0;
	rtm->rtm_tos      = 0;
	rtm->rtm_table    = RT6_TABLE_UNSPEC;
	rtm->rtm_type     = RTN_UNICAST;
	rtm->rtm_scope    = RT_SCOPE_UNIVERSE;

	if (((lwt->notify & ILA_NOTIFY_DST) &&
	     nla_put_in6_addr(nlskb, RTA_DST, &ip6h->daddr)) ||
	    ((lwt->notify & ILA_NOTIFY_SRC) &&
	     nla_put_in6_addr(nlskb, RTA_SRC, &ip6h->saddr))) {
		nlmsg_cancel(nlskb, nlh);
		err = -EMSGSIZE;
		goto errout;
	}

	nlmsg_end(nlskb, nlh);

	rtnl_notify(nlskb, net, 0, RTNLGRP_ILA_NOTIFY, NULL, GFP_ATOMIC);

	return;

errout:
	kfree_skb(nlskb);
	WARN_ON(err == -EMSGSIZE);
	rtnl_set_sk_err(net, RTNLGRP_ILA_NOTIFY, err);
}

static int ila_output(struct net *net, struct sock *sk, struct sk_buff *skb)
{
	struct dst_entry *orig_dst = skb_dst(skb);
	struct rt6_info *rt = (struct rt6_info *)orig_dst;
	struct ila_lwt *ilwt = ila_lwt_lwtunnel(orig_dst->lwtstate);
	struct dst_entry *dst;
	int err = -EINVAL;

	if (skb->protocol != htons(ETH_P_IPV6))
		goto drop;

	if (ilwt->xlat)
		ila_update_ipv6_locator(skb,
					ila_params_lwtunnel(orig_dst->lwtstate),
					true);

	if (ilwt->notify)
		ila_notify(net, skb, ilwt);

	if (rt->rt6i_flags & (RTF_GATEWAY | RTF_CACHE)) {
		/* Already have a next hop address in route, no need for
		 * dest cache route.
		 */
		return orig_dst->lwtstate->orig_output(net, sk, skb);
	}

	dst = dst_cache_get(&ilwt->dst_cache);
	if (unlikely(!dst)) {
		struct ipv6hdr *ip6h = ipv6_hdr(skb);
		struct flowi6 fl6;

		/* Lookup a route for the new destination. Take into
		 * account that the base route may already have a gateway.
		 */

		memset(&fl6, 0, sizeof(fl6));
		fl6.flowi6_oif = orig_dst->dev->ifindex;
		fl6.flowi6_iif = LOOPBACK_IFINDEX;
		fl6.daddr = *rt6_nexthop((struct rt6_info *)orig_dst,
					 &ip6h->daddr);

		dst = ip6_route_output(net, NULL, &fl6);
		if (dst->error) {
			err = -EHOSTUNREACH;
			dst_release(dst);
			goto drop;
		}

		dst = xfrm_lookup(net, dst, flowi6_to_flowi(&fl6), NULL, 0);
		if (IS_ERR(dst)) {
			err = PTR_ERR(dst);
			goto drop;
		}

		if (ilwt->connected)
			dst_cache_set_ip6(&ilwt->dst_cache, dst, &fl6.saddr);
	}

	skb_dst_set(skb, dst);
	return dst_output(net, sk, skb);

drop:
	kfree_skb(skb);
	return err;
}

static int ila_input(struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);
	struct ila_lwt *ilwt = ila_lwt_lwtunnel(dst->lwtstate);

	if (skb->protocol != htons(ETH_P_IPV6))
		goto drop;

	if (ilwt->xlat)
		ila_update_ipv6_locator(skb,
					ila_params_lwtunnel(dst->lwtstate),
					false);

	if (ilwt->notify)
		ila_notify(dev_net(dst->dev), skb, ilwt);

	return dst->lwtstate->orig_input(skb);

drop:
	kfree_skb(skb);
	return -EINVAL;
}

static const struct nla_policy ila_nl_policy[ILA_ATTR_MAX + 1] = {
	[ILA_ATTR_LOCATOR] = { .type = NLA_U64, },
	[ILA_ATTR_CSUM_MODE] = { .type = NLA_U8, },
	[ILA_ATTR_IDENT_TYPE] = { .type = NLA_U8, },
	[ILA_ATTR_HOOK_TYPE] = { .type = NLA_U8, },
	[ILA_ATTR_NOTIFY_DST] = { .type = NLA_FLAG },
	[ILA_ATTR_NOTIFY_SRC] = { .type = NLA_FLAG },
};

static int ila_build_state(struct net *net, struct nlattr *nla,
			   unsigned int family, const void *cfg,
			   struct lwtunnel_state **ts,
			   struct netlink_ext_ack *extack)
{
	const struct fib6_config *cfg6 = cfg;
	struct ila_addr *iaddr = (struct ila_addr *)&cfg6->fc_dst;
	u8 ident_type = ILA_ATYPE_USE_FORMAT;
	u8 hook_type = ILA_HOOK_ROUTE_OUTPUT;
	struct nlattr *tb[ILA_ATTR_MAX + 1];
	u8 csum_mode = ILA_CSUM_NO_ACTION;
	struct lwtunnel_state *newts;
	struct ila_lwt *ilwt;
	struct ila_params *p;
	u8 eff_ident_type;
	int err;

	if (family != AF_INET6)
		return -EINVAL;

	err = nla_parse_nested(tb, ILA_ATTR_MAX, nla, ila_nl_policy, extack);
	if (err < 0)
		return err;

	if (tb[ILA_ATTR_LOCATOR]) {
		/* Doing ILA translation */

		if (tb[ILA_ATTR_IDENT_TYPE])
			ident_type = nla_get_u8(tb[ILA_ATTR_IDENT_TYPE]);

		if (ident_type == ILA_ATYPE_USE_FORMAT) {
			/* Infer identifier type from type field in formatted
			 * identifier.
			 */

			if (cfg6->fc_dst_len < 8 *
			    sizeof(struct ila_locator) + 3) {
				/* Need to have full locator and at least type
				 * field included in destination
				 */
				return -EINVAL;
			}

			eff_ident_type = iaddr->ident.type;
		} else {
			eff_ident_type = ident_type;
		}

		switch (eff_ident_type) {
		case ILA_ATYPE_IID:
			/* Don't allow ILA for IID type */
			return -EINVAL;
		case ILA_ATYPE_LUID:
			break;
		case ILA_ATYPE_VIRT_V4:
		case ILA_ATYPE_VIRT_UNI_V6:
		case ILA_ATYPE_VIRT_MULTI_V6:
		case ILA_ATYPE_NONLOCAL_ADDR:
			/* These ILA formats are not supported yet. */
		default:
			return -EINVAL;
		}

		csum_mode = nla_get_u8(tb[ILA_ATTR_CSUM_MODE]);

		if (csum_mode == ILA_CSUM_NEUTRAL_MAP &&
		    ila_csum_neutral_set(iaddr->ident)) {
			/* Don't allow translation if checksum neutral bit is
			 * configured and it's set in the SIR address.
			 */
			return -EINVAL;
		}
	}

	if (tb[ILA_ATTR_HOOK_TYPE])
		hook_type = nla_get_u8(tb[ILA_ATTR_HOOK_TYPE]);

	switch (hook_type) {
	case ILA_HOOK_ROUTE_OUTPUT:
	case ILA_HOOK_ROUTE_INPUT:
		break;
	default:
		return -EINVAL;
	}

	newts = lwtunnel_state_alloc(sizeof(*ilwt));
	if (!newts)
		return -ENOMEM;

	ilwt = ila_lwt_lwtunnel(newts);

	err = dst_cache_init(&ilwt->dst_cache, GFP_ATOMIC);
	if (err) {
		kfree(newts);
		return err;
	}

	newts->type = LWTUNNEL_ENCAP_ILA;

	switch (hook_type) {
	case ILA_HOOK_ROUTE_OUTPUT:
		newts->flags |= LWTUNNEL_STATE_OUTPUT_REDIRECT;
		break;
	case ILA_HOOK_ROUTE_INPUT:
		newts->flags |= LWTUNNEL_STATE_INPUT_REDIRECT;
		break;
	}

	ilwt->hook_type = hook_type;

	if (tb[ILA_ATTR_NOTIFY_DST])
		ilwt->notify |= ILA_NOTIFY_DST;

	if (tb[ILA_ATTR_NOTIFY_SRC])
		ilwt->notify |= ILA_NOTIFY_SRC;

	p = ila_params_lwtunnel(newts);

	if (tb[ILA_ATTR_LOCATOR]) {
		ilwt->xlat = true;
		p->csum_mode = csum_mode;
		p->ident_type = ident_type;
		p->locator.v64 = (__force __be64)nla_get_u64(
							tb[ILA_ATTR_LOCATOR]);

		/* Precompute checksum difference for translation since we
		 * know both the old locator and the new one.
		 */
		p->locator_match = iaddr->loc;

		ila_init_saved_csum(p);

		if (cfg6->fc_dst_len == 8 * sizeof(struct in6_addr))
			ilwt->connected = 1;
	}

	*ts = newts;

	return 0;
}

static void ila_destroy_state(struct lwtunnel_state *lwt)
{
	dst_cache_destroy(&ila_lwt_lwtunnel(lwt)->dst_cache);
}

static int ila_fill_encap_info(struct sk_buff *skb,
			       struct lwtunnel_state *lwtstate)
{
	struct ila_params *p = ila_params_lwtunnel(lwtstate);
	struct ila_lwt *ilwt = ila_lwt_lwtunnel(lwtstate);

	if (ilwt->xlat) {
		if (nla_put_u64_64bit(skb, ILA_ATTR_LOCATOR,
				      (__force u64)p->locator.v64,
				      ILA_ATTR_PAD))
		goto nla_put_failure;

		if (nla_put_u8(skb, ILA_ATTR_CSUM_MODE,
			       (__force u8)p->csum_mode))
			goto nla_put_failure;

		if (nla_put_u8(skb, ILA_ATTR_IDENT_TYPE,
			       (__force u8)p->ident_type))
			goto nla_put_failure;
	}

	if (nla_put_u8(skb, ILA_ATTR_HOOK_TYPE, ilwt->hook_type))
		goto nla_put_failure;

	if (ilwt->notify & ILA_NOTIFY_DST)
		if (nla_put_flag(skb, ILA_ATTR_NOTIFY_DST))
			goto nla_put_failure;

	if (ilwt->notify & ILA_NOTIFY_SRC)
		if (nla_put_flag(skb, ILA_ATTR_NOTIFY_SRC))
			goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static int ila_encap_nlsize(struct lwtunnel_state *lwtstate)
{
	return nla_total_size_64bit(sizeof(u64)) + /* ILA_ATTR_LOCATOR */
	       nla_total_size(sizeof(u8)) +        /* ILA_ATTR_CSUM_MODE */
	       nla_total_size(sizeof(u8)) +        /* ILA_ATTR_IDENT_TYPE */
	       nla_total_size(sizeof(u8)) +        /* ILA_ATTR_HOOK_TYPE */
	       nla_total_size(0) +		   /* ILA_ATTR_NOTIFY_DST */
	       nla_total_size(0) +		   /* ILA_ATTR_NOTIFY_SRC */
	       0;
}

static int ila_encap_cmp(struct lwtunnel_state *a, struct lwtunnel_state *b)
{
	struct ila_params *a_p = ila_params_lwtunnel(a);
	struct ila_params *b_p = ila_params_lwtunnel(b);

	return (a_p->locator.v64 != b_p->locator.v64);
}

static const struct lwtunnel_encap_ops ila_encap_ops = {
	.build_state = ila_build_state,
	.destroy_state = ila_destroy_state,
	.output = ila_output,
	.input = ila_input,
	.fill_encap = ila_fill_encap_info,
	.get_encap_size = ila_encap_nlsize,
	.cmp_encap = ila_encap_cmp,
	.owner = THIS_MODULE,
};

int ila_lwt_init(void)
{
	return lwtunnel_encap_add_ops(&ila_encap_ops, LWTUNNEL_ENCAP_ILA);
}

void ila_lwt_fini(void)
{
	lwtunnel_encap_del_ops(&ila_encap_ops, LWTUNNEL_ENCAP_ILA);
}
