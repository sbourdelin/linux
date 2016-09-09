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

struct ila_notify {
	int type;
	struct in6_addr addr;
};

#define ILA_NOTIFY_SIR_DEST 1

static int ila_fill_notify(struct sk_buff *skb, struct in6_addr *addr,
			   u32 pid, u32 seq, int event, int flags)
{
	struct ila_notify *nila;
	struct nlmsghdr *nlh;

	nlh = nlmsg_put(skb, pid, seq, event, sizeof(*nila), flags);
	if (!nlh)
		return -EMSGSIZE;

	nila = nlmsg_data(nlh);
	nila->type = ILA_NOTIFY_SIR_DEST;
	nila->addr = *addr;

	nlmsg_end(skb, nlh);

	return 0;
}

void ila_rslv_notify(struct net *net, struct sk_buff *skb)
{
	struct ipv6hdr *ip6h = ipv6_hdr(skb);
	struct sk_buff *nlskb;
	int err = 0;

	/* Send ILA notification to user */
	nlskb = nlmsg_new(NLMSG_ALIGN(sizeof(struct ila_notify) +
			nlmsg_total_size(1)), GFP_KERNEL);
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
	struct net_rslv_ent *nrent;
	struct ipv6hdr *ip6h = ipv6_hdr(skb);
	bool new;

	/* Don't bother taking rcu lock, we only want to know if the entry
	 * exists or not.
	 */
	nrent = net_rslv_lookup_and_create(ilan->nrslv, &ip6h->daddr, &new);

	if (nrent && new)
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

static int ila_rslv_build_state(struct net_device *dev, struct nlattr *nla,
				unsigned int family, const void *cfg,
				struct lwtunnel_state **ts)
{
	struct lwtunnel_state *newts;
	struct ila_net *ilan = net_generic(dev_net(dev), ila_net_id);

	if (unlikely(!ilan->nrslv)) {
		int err;

		/* Only create net resolver on demand */
		err = ila_init_resolver_net(ilan);
		if (err)
			return err;
	}

	if (family != AF_INET6)
		return -EINVAL;

	newts = lwtunnel_state_alloc(0);
	if (!newts)
		return -ENOMEM;

	newts->len = 0;
	newts->type = LWTUNNEL_ENCAP_ILA_NOTIFY;
	newts->flags |= LWTUNNEL_STATE_OUTPUT_REDIRECT |
			LWTUNNEL_STATE_INPUT_REDIRECT;

	*ts = newts;

	return 0;
}

static int ila_rslv_fill_encap_info(struct sk_buff *skb,
				    struct lwtunnel_state *lwtstate)
{
	return 0;
}

static int ila_rslv_nlsize(struct lwtunnel_state *lwtstate)
{
	return 0;
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

#define ILA_RESOLVER_TIMEOUT 100
#define ILA_MAX_SIZE 8192

int ila_init_resolver_net(struct ila_net *ilan)
{
	ilan->nrslv = net_rslv_create(sizeof(struct ila_addr),
				      sizeof(struct ila_addr), ILA_MAX_SIZE,
				      ILA_RESOLVER_TIMEOUT, NULL, NULL, NULL);

	if (!ilan->nrslv)
		return -ENOMEM;

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
