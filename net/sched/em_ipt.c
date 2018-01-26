/*
 * net/sched/em_ipt.c IPtables matches Ematch
 *
 * (c) 2018 Eyal Birger <eyal.birger@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/gfp.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/skbuff.h>
#include <linux/ip.h>
#include <linux/tc_ematch/tc_em_ipt.h>
#include <linux/netfilter.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <net/pkt_cls.h>
#include <net/ip.h>

struct em_ipt_match {
	const struct xt_match *match;
	u32 hook;
	u8 nfproto;
	u8 match_data[0] __aligned(8);
};

static int check_match(struct net *net, struct em_ipt_match *im, int mdata_len)
{
	struct xt_mtchk_param mtpar = {};
	union {
		struct ipt_entry e4;
		struct ip6t_entry e6;
	} e = {};

	mtpar.net	= net;
	mtpar.table	= "filter";
	mtpar.hook_mask	= 1 << im->hook;
	mtpar.family	= im->nfproto;
	mtpar.match	= im->match;
	mtpar.entryinfo = &e;
	mtpar.matchinfo	= (void *)im->match_data;
	return xt_check_match(&mtpar, mdata_len, 0, 0);
}

static const struct nla_policy em_ipt_policy[TCA_EM_IPT_MAX + 1] = {
	[TCA_EM_IPT_HOOK]		= { .type = NLA_U32 },
	[TCA_EM_IPT_MATCH_NAME]		= { .type = NLA_STRING,
					    .len = XT_EXTENSION_MAXNAMELEN },
	[TCA_EM_IPT_MATCH_REVISION]	= { .type = NLA_U8 },
	[TCA_EM_IPT_MATCH_DATA]		= { .type = NLA_UNSPEC },
};

static int em_ipt_change(struct net *net, __be16 protocol, void *data,
			 int data_len, struct tcf_ematch *em)
{
	struct nlattr *tb[TCA_EM_IPT_MAX + 1];
	struct em_ipt_match *im = NULL;
	struct xt_match *match;
	u8 nfproto, mrev = 0;
	int mdata_len, ret;
	const char *mname;

	switch (protocol) {
	case htons(ETH_P_IP):
		nfproto = NFPROTO_IPV4;
		break;
	case htons(ETH_P_IPV6):
		nfproto = NFPROTO_IPV6;
		break;
	default:
		return -EINVAL;
	}

	ret = nla_parse(tb, TCA_EM_IPT_MAX, data, data_len, em_ipt_policy,
			NULL);
	if (ret < 0)
		return ret;

	if (!tb[TCA_EM_IPT_HOOK] || !tb[TCA_EM_IPT_MATCH_NAME] ||
	    !tb[TCA_EM_IPT_MATCH_DATA])
		return -EINVAL;

	mname = nla_data(tb[TCA_EM_IPT_MATCH_NAME]);
	if (tb[TCA_EM_IPT_MATCH_REVISION])
		mrev = nla_get_u8(tb[TCA_EM_IPT_MATCH_REVISION]);

	match = xt_request_find_match(nfproto, mname, mrev);
	if (IS_ERR(match)) {
		pr_err("unable to find match %s:%d\n", mname, mrev);
		return PTR_ERR(match);
	}

	mdata_len = XT_ALIGN(nla_len(tb[TCA_EM_IPT_MATCH_DATA]));
	im = kzalloc(sizeof(*im) + mdata_len, GFP_KERNEL);
	if (!im) {
		ret = -ENOMEM;
		goto err;
	}

	im->match = match;
	im->hook = nla_get_u32(tb[TCA_EM_IPT_HOOK]);
	im->nfproto = nfproto;
	nla_memcpy(im->match_data, tb[TCA_EM_IPT_MATCH_DATA], mdata_len);

	ret = check_match(net, im, mdata_len);
	if (ret)
		goto err;

	em->datalen = sizeof(*im) + mdata_len;
	em->data = (unsigned long)im;
	return 0;

err:
	kfree(im);
	module_put(match->me);
	return ret;
}

static void em_ipt_destroy(struct tcf_ematch *em)
{
	struct em_ipt_match *im = (void *)em->data;

	if (!im)
		return;

	if (im->match->destroy) {
		struct xt_mtdtor_param par = {
			.net = em->net,
			.match = im->match,
			.matchinfo = im->match_data,
			.family = im->nfproto
		};
		im->match->destroy(&par);
	}
	module_put(im->match->me);
	kfree((void *)im);
}

static int em_ipt_match(struct sk_buff *skb, struct tcf_ematch *em,
			struct tcf_pkt_info *info)
{
	const struct em_ipt_match *im = (const void *)em->data;
	struct xt_action_param acpar = {};
	struct net_device *indev = NULL;
	struct nf_hook_state state;
	int ret;

	if (unlikely(!skb_at_tc_ingress(skb))) {
		pr_notice_once("ipt match must not be used at egress\n");
		return 0;
	}

	switch (tc_skb_protocol(skb)) {
	case htons(ETH_P_IP):
		if (im->nfproto != NFPROTO_IPV4)
			return 0;
		if (!pskb_network_may_pull(skb, sizeof(struct iphdr)))
			return 0;
		acpar.thoff = ip_hdrlen(skb);
		break;
	case htons(ETH_P_IPV6):
		if (im->nfproto != NFPROTO_IPV6)
			return 0;
		if (!pskb_network_may_pull(skb, sizeof(struct ipv6hdr)))
			return 0;
		if (ipv6_find_hdr(skb, &acpar.thoff, -1,
				  (unsigned short *)&acpar.fragoff, NULL) < 0)
			return 0;
		break;
	default:
		return 0;
	}

	rcu_read_lock();

	if (skb->skb_iif)
		indev = dev_get_by_index_rcu(em->net, skb->skb_iif);

	nf_hook_state_init(&state, im->hook, im->nfproto, indev ?: skb->dev,
			   skb->dev, NULL, em->net, NULL);

	acpar.match = im->match;
	acpar.matchinfo = im->match_data;
	acpar.state = &state;

	ret = im->match->match(skb, &acpar);

	rcu_read_unlock();
	return ret;
}

static int em_ipt_dump(struct sk_buff *skb, struct tcf_ematch *em)
{
	struct em_ipt_match *im = (void *)em->data;

	if (nla_put_u32(skb, TCA_EM_IPT_HOOK, im->hook) < 0)
		return -EMSGSIZE;
	if (nla_put_string(skb, TCA_EM_IPT_MATCH_NAME, im->match->name) < 0)
		return -EMSGSIZE;
	if (nla_put_u8(skb, TCA_EM_IPT_MATCH_REVISION, im->match->revision) < 0)
		return -EMSGSIZE;
	if (nla_put(skb, TCA_EM_IPT_MATCH_DATA,
		    im->match->usersize ?: im->match->matchsize,
		    im->match_data) < 0)
		return -EMSGSIZE;

	return 0;
}

static struct tcf_ematch_ops em_ipt_ops = {
	.kind	  = TCF_EM_IPT,
	.change	  = em_ipt_change,
	.destroy  = em_ipt_destroy,
	.match	  = em_ipt_match,
	.dump	  = em_ipt_dump,
	.owner	  = THIS_MODULE,
	.link	  = LIST_HEAD_INIT(em_ipt_ops.link)
};

static int __init init_em_ipt(void)
{
	return tcf_em_register(&em_ipt_ops);
}

static void __exit exit_em_ipt(void)
{
	tcf_em_unregister(&em_ipt_ops);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Eyal Birger <eyal.birger@gmail.com>");
MODULE_DESCRIPTION("TC extended match for IPtables matches");

module_init(init_em_ipt);
module_exit(exit_em_ipt);

MODULE_ALIAS_TCF_EMATCH(TCF_EM_IPT);
