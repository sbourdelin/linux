#include <linux/errno.h>
#include <linux/ip.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/types.h>
#include <net/checksum.h>
#include <net/ip.h>
#include <net/ip6_fib.h>
#include <net/lwtunnel.h>
#include <net/netns/generic.h>
#include <net/protocol.h>
#include <net/resolver.h>
#include <uapi/linux/ila.h>
#include "ila.h"

struct ila_notify_params {
	unsigned int timeout;
};

static inline struct ila_notify_params *ila_notify_params_lwtunnel(
	struct lwtunnel_state *lwstate)
{
	return (struct ila_notify_params *)lwstate->data;
}

static int ila_fill_notify(struct sk_buff *skb, struct in6_addr *addr,
			   u32 pid, u32 seq, int event, int flags)
{
	struct nlmsghdr *nlh;
	struct rtmsg *rtm;

	nlh = nlmsg_put(skb, pid, seq, event, sizeof(*rtm), flags);
	if (!nlh)
		return -EMSGSIZE;

	rtm = nlmsg_data(nlh);
	rtm->rtm_family   = RTNL_FAMILY_ILA;
	rtm->rtm_dst_len  = 128;
	rtm->rtm_src_len  = 0;
	rtm->rtm_tos      = 0;
	rtm->rtm_table    = RT6_TABLE_UNSPEC;
	rtm->rtm_type     = RTN_UNICAST;
	rtm->rtm_scope    = RT_SCOPE_UNIVERSE;

	if (nla_put_in6_addr(skb, RTA_DST, addr)) {
		nlmsg_cancel(skb, nlh);
		return -EMSGSIZE;
	}

	nlmsg_end(skb, nlh);
	return 0;
}

static size_t ila_rslv_msgsize(void)
{
	size_t len =
		NLMSG_ALIGN(sizeof(struct rtmsg))
		+ nla_total_size(16)     /* RTA_DST */
		;

	return len;
}

void ila_rslv_notify(struct net *net, struct sk_buff *skb)
{
	struct ipv6hdr *ip6h = ipv6_hdr(skb);
	struct sk_buff *nlskb;
	int err = 0;

	/* Send ILA notification to user */
	nlskb = nlmsg_new(ila_rslv_msgsize(), GFP_KERNEL);
	if (!nlskb)
		goto errout;

	err = ila_fill_notify(nlskb, &ip6h->daddr, 0, 0, RTM_ADDR_RESOLVE,
			      NLM_F_MULTI);
	if (err < 0) {
		WARN_ON(err == -EMSGSIZE);
		kfree_skb(nlskb);
		goto errout;
	}
	rtnl_notify(nlskb, net, 0, RTNLGRP_ILA_NOTIFY, NULL, GFP_ATOMIC);
	return;

errout:
	if (err < 0)
		rtnl_set_sk_err(net, RTNLGRP_ILA_NOTIFY, err);
}

static int ila_rslv_output(struct net *net, struct sock *sk,
			   struct sk_buff *skb)
{
	struct ila_net *ilan = net_generic(net, ila_net_id);
	struct dst_entry *dst = skb_dst(skb);
	struct ipv6hdr *ip6h = ipv6_hdr(skb);
	struct ila_notify_params *p;
	bool new;

	p = ila_notify_params_lwtunnel(dst->lwtstate);

	/* Don't bother taking rcu lock, we only want to know if the entry
	 * exists or not.
	 */
	net_rslv_lookup_and_create(ilan->nrslv, &ip6h->daddr, &new,
				   p->timeout);

	if (new)
		ila_rslv_notify(net, skb);

	return dst->lwtstate->orig_output(net, sk, skb);
}

void ila_rslv_resolved(struct ila_net *ilan, struct ila_addr *iaddr)
{
	if (ilan->nrslv)
		net_rslv_resolved(ilan->nrslv, iaddr);
}

static int ila_rslv_input(struct sk_buff *skb)
{
	struct dst_entry *dst = skb_dst(skb);

	return dst->lwtstate->orig_input(skb);
}

static const struct nla_policy ila_notify_nl_policy[ILA_NOTIFY_ATTR_MAX + 1] = {
	[ILA_NOTIFY_ATTR_TIMEOUT] = { .type = NLA_U32, },
};

static int ila_rslv_build_state(struct net *net, struct net_device *dev,
				struct nlattr *nla, unsigned int family,
				const void *cfg, struct lwtunnel_state **ts)
{
	struct ila_notify_params *p;
	struct nlattr *tb[ILA_NOTIFY_ATTR_MAX + 1];
	struct lwtunnel_state *newts;
	struct ila_net *ilan = net_generic(net, ila_net_id);
	size_t encap_len = sizeof(*p);
	int ret;

	if (unlikely(!ilan->nrslv)) {
		int err;

		/* Only create net resolver on demand */
		err = ila_init_resolver_net(ilan);
		if (err)
			return err;
	}

	if (family != AF_INET6)
		return -EINVAL;

	ret = nla_parse_nested(tb, ILA_NOTIFY_ATTR_MAX, nla,
			       ila_notify_nl_policy);

	if (ret < 0)
		return ret;

	newts = lwtunnel_state_alloc(encap_len);
	if (!newts)
		return -ENOMEM;

	newts->len = 0;
	newts->type = LWTUNNEL_ENCAP_ILA_NOTIFY;
	newts->flags |= LWTUNNEL_STATE_OUTPUT_REDIRECT |
			LWTUNNEL_STATE_INPUT_REDIRECT;

	p = ila_notify_params_lwtunnel(newts);

	if (tb[ILA_NOTIFY_ATTR_TIMEOUT])
		p->timeout = msecs_to_jiffies(nla_get_u32(
			tb[ILA_NOTIFY_ATTR_TIMEOUT]));

	*ts = newts;

	return 0;
}

static int ila_rslv_fill_encap_info(struct sk_buff *skb,
				    struct lwtunnel_state *lwtstate)
{
	struct ila_notify_params *p = ila_notify_params_lwtunnel(lwtstate);

	if (nla_put_u32(skb, ILA_NOTIFY_ATTR_TIMEOUT,
			(__force u32)jiffies_to_msecs(p->timeout)))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static int ila_rslv_nlsize(struct lwtunnel_state *lwtstate)
{
	return nla_total_size(sizeof(u32)) + /* ILA_NOTIFY_ATTR_TIMEOUT */
	       0;
}

static int ila_rslv_cmp(struct lwtunnel_state *a, struct lwtunnel_state *b)
{
	return 0;
}

static const struct lwtunnel_encap_ops ila_rslv_ops = {
	.build_state = ila_rslv_build_state,
	.output = ila_rslv_output,
	.input = ila_rslv_input,
	.fill_encap = ila_rslv_fill_encap_info,
	.get_encap_size = ila_rslv_nlsize,
	.cmp_encap = ila_rslv_cmp,
};

#define ILA_MAX_SIZE 8192

int ila_init_resolver_net(struct ila_net *ilan)
{
	struct net_rslv *nrslv;

	nrslv = net_rslv_create(sizeof(struct ila_addr),
				sizeof(struct ila_addr), ILA_MAX_SIZE,
				NULL, NULL, NULL);

	if (IS_ERR(nrslv))
		return PTR_ERR(nrslv);

	ilan->nrslv = nrslv;

	return 0;
}

void ila_exit_resolver_net(struct ila_net *ilan)
{
	if (ilan->nrslv)
		net_rslv_destroy(ilan->nrslv);
}

int ila_rslv_init(void)
{
	return lwtunnel_encap_add_ops(&ila_rslv_ops, LWTUNNEL_ENCAP_ILA_NOTIFY);
}

void ila_rslv_fini(void)
{
	lwtunnel_encap_del_ops(&ila_rslv_ops, LWTUNNEL_ENCAP_ILA_NOTIFY);
}
