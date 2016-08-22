/*
 * Copyright (c) 2016, Amir Vadai <amir@vadai.me>
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/skbuff.h>
#include <linux/rtnetlink.h>
#include <net/netlink.h>
#include <net/pkt_sched.h>
#include <net/dst.h>
#include <net/dst_metadata.h>

#include <linux/tc_act/tc_iptunnel.h>
#include <net/tc_act/tc_iptunnel.h>

#define IPTUNNEL_TAB_MASK     15

static int iptunnel_net_id;
static struct tc_action_ops act_iptunnel_ops;

static int tcf_iptunnel(struct sk_buff *skb, const struct tc_action *a,
			struct tcf_result *res)
{
	struct tcf_iptunnel *t = to_iptunnel(a);
	int action;

	spin_lock(&t->tcf_lock);
	tcf_lastuse_update(&t->tcf_tm);
	bstats_update(&t->tcf_bstats, skb);
	action = t->tcf_action;

	switch (t->tcft_action) {
	case TCA_IPTUNNEL_ACT_DECAP:
		skb_dst_set_noref(skb, NULL);
		break;
	case TCA_IPTUNNEL_ACT_ENCAP:
		skb_dst_set_noref(skb, &t->tcft_enc_metadata->dst);

		break;
	default:
		BUG();
	}

	spin_unlock(&t->tcf_lock);
	return action;
}

static const struct nla_policy iptunnel_policy[TCA_IPTUNNEL_MAX + 1] = {
	[TCA_IPTUNNEL_PARMS]	    = { .len = sizeof(struct tc_iptunnel) },
	[TCA_IPTUNNEL_ENC_IPV4_SRC] = { .type = NLA_U32 },
	[TCA_IPTUNNEL_ENC_IPV4_DST] = { .type = NLA_U32 },
	[TCA_IPTUNNEL_ENC_KEY_ID]   = { .type = NLA_U32 },
};

static struct metadata_dst *iptunnel_alloc(struct tcf_iptunnel *t,
					   __be32 saddr, __be32 daddr,
					   __be64 key_id)
{
	struct ip_tunnel_info *tun_info;
	struct metadata_dst *metadata;

	metadata = metadata_dst_alloc(0, GFP_KERNEL);
	if (!metadata)
		return ERR_PTR(-ENOMEM);

	tun_info = &metadata->u.tun_info;
	tun_info->mode = IP_TUNNEL_INFO_TX;

	ip_tunnel_key_init(&tun_info->key, saddr, daddr, 0, 0, 0, 0, 0,
			   key_id, 0);

	return metadata;
}

static int tcf_iptunnel_init(struct net *net, struct nlattr *nla,
			     struct nlattr *est, struct tc_action **a,
			     int ovr, int bind)
{
	struct tc_action_net *tn = net_generic(net, iptunnel_net_id);
	struct nlattr *tb[TCA_IPTUNNEL_MAX + 1];
	struct metadata_dst *metadata;
	struct tc_iptunnel *parm;
	struct tcf_iptunnel *t;
	__be32 saddr = 0;
	__be32 daddr = 0;
	__be64 key_id = 0;
	int encapdecap;
	bool exists = false;
	int ret = -EINVAL;
	int err;

	if (!nla)
		return -EINVAL;

	err = nla_parse_nested(tb, TCA_IPTUNNEL_MAX, nla, iptunnel_policy);
	if (err < 0)
		return err;

	if (!tb[TCA_IPTUNNEL_PARMS])
		return -EINVAL;
	parm = nla_data(tb[TCA_IPTUNNEL_PARMS]);
	exists = tcf_hash_check(tn, parm->index, a, bind);
	if (exists && bind)
		return 0;

	encapdecap = parm->t_action;

	switch (encapdecap) {
	case TCA_IPTUNNEL_ACT_DECAP:
		break;
	case TCA_IPTUNNEL_ACT_ENCAP:
		if (tb[TCA_IPTUNNEL_ENC_IPV4_SRC])
			saddr = nla_get_be32(tb[TCA_IPTUNNEL_ENC_IPV4_SRC]);
		if (tb[TCA_IPTUNNEL_ENC_IPV4_DST])
			daddr = nla_get_be32(tb[TCA_IPTUNNEL_ENC_IPV4_DST]);
		if (tb[TCA_IPTUNNEL_ENC_KEY_ID])
			key_id = key32_to_tunnel_id(nla_get_be32(tb[TCA_IPTUNNEL_ENC_KEY_ID]));

		if (!saddr || !daddr || !key_id) {
			ret = -EINVAL;
			goto err_out;
		}

		metadata = iptunnel_alloc(t, saddr, daddr, key_id);
		if (IS_ERR(metadata)) {
			ret = PTR_ERR(metadata);
			goto err_out;
		}

		break;
	default:
		goto err_out;
	}

	if (!exists) {
		ret = tcf_hash_create(tn, parm->index, est, a,
				      &act_iptunnel_ops, bind, false);
		if (ret)
			return ret;

		ret = ACT_P_CREATED;
	} else {
		tcf_hash_release(*a, bind);
		if (!ovr)
			return -EEXIST;
	}

	t = to_iptunnel(*a);

	spin_lock_bh(&t->tcf_lock);

	t->tcf_action = parm->action;

	t->tcft_action = encapdecap;
	t->tcft_enc_metadata = metadata;

	spin_unlock_bh(&t->tcf_lock);

	if (ret == ACT_P_CREATED)
		tcf_hash_insert(tn, *a);

	return ret;

err_out:
	if (exists)
		tcf_hash_release(*a, bind);
	return ret;
}

static void tcf_iptunnel_release(struct tc_action *a, int bind)
{
	struct tcf_iptunnel *t = to_iptunnel(a);

	if (t->tcft_action == TCA_IPTUNNEL_ACT_ENCAP)
		dst_release(&t->tcft_enc_metadata->dst);
}

static int tcf_iptunnel_dump(struct sk_buff *skb, struct tc_action *a,
			     int bind, int ref)
{
	unsigned char *b = skb_tail_pointer(skb);
	struct tcf_iptunnel *t = to_iptunnel(a);
	struct tc_iptunnel opt = {
		.index    = t->tcf_index,
		.refcnt   = t->tcf_refcnt - ref,
		.bindcnt  = t->tcf_bindcnt - bind,
		.action   = t->tcf_action,
		.t_action = t->tcft_action,
	};
	struct tcf_t tm;

	if (nla_put(skb, TCA_IPTUNNEL_PARMS, sizeof(opt), &opt))
		goto nla_put_failure;

	if (t->tcft_action == TCA_IPTUNNEL_ACT_ENCAP) {
		struct ip_tunnel_key *key =
			&t->tcft_enc_metadata->u.tun_info.key;
		__be32 saddr = key->u.ipv4.src;
		__be32 daddr = key->u.ipv4.dst;
		__be32 key_id = tunnel_id_to_key32(key->tun_id);

		if (nla_put_be32(skb, TCA_IPTUNNEL_ENC_IPV4_SRC, saddr) ||
		    nla_put_be32(skb, TCA_IPTUNNEL_ENC_IPV4_DST, daddr) ||
		    nla_put_be32(skb, TCA_IPTUNNEL_ENC_KEY_ID, key_id))
			goto nla_put_failure;
	}

	tcf_tm_dump(&tm, &t->tcf_tm);
	if (nla_put_64bit(skb, TCA_IPTUNNEL_TM, sizeof(tm), &tm, TCA_IPTUNNEL_PAD))
		goto nla_put_failure;

	return skb->len;

nla_put_failure:
	nlmsg_trim(skb, b);
	return -1;
}

static int tcf_iptunnel_walker(struct net *net, struct sk_buff *skb,
			       struct netlink_callback *cb, int type,
			       const struct tc_action_ops *ops)
{
	struct tc_action_net *tn = net_generic(net, iptunnel_net_id);

	return tcf_generic_walker(tn, skb, cb, type, ops);
}

static int tcf_iptunnel_search(struct net *net, struct tc_action **a, u32 index)
{
	struct tc_action_net *tn = net_generic(net, iptunnel_net_id);

	return tcf_hash_search(tn, a, index);
}

static struct tc_action_ops act_iptunnel_ops = {
	.kind		=	"iptunnel",
	.type		=	TCA_ACT_IPTUNNEL,
	.owner		=	THIS_MODULE,
	.act		=	tcf_iptunnel,
	.dump		=	tcf_iptunnel_dump,
	.init		=	tcf_iptunnel_init,
	.cleanup	=	tcf_iptunnel_release,
	.walk		=	tcf_iptunnel_walker,
	.lookup		=	tcf_iptunnel_search,
	.size		=	sizeof(struct tcf_iptunnel),
};

static __net_init int iptunnel_init_net(struct net *net)
{
	struct tc_action_net *tn = net_generic(net, iptunnel_net_id);

	return tc_action_net_init(tn, &act_iptunnel_ops, IPTUNNEL_TAB_MASK);
}

static void __net_exit iptunnel_exit_net(struct net *net)
{
	struct tc_action_net *tn = net_generic(net, iptunnel_net_id);

	tc_action_net_exit(tn);
}

static struct pernet_operations iptunnel_net_ops = {
	.init = iptunnel_init_net,
	.exit = iptunnel_exit_net,
	.id   = &iptunnel_net_id,
	.size = sizeof(struct tc_action_net),
};

static int __init iptunnel_init_module(void)
{
	return tcf_register_action(&act_iptunnel_ops, &iptunnel_net_ops);
}

static void __exit iptunnel_cleanup_module(void)
{
	tcf_unregister_action(&act_iptunnel_ops, &iptunnel_net_ops);
}

module_init(iptunnel_init_module);
module_exit(iptunnel_cleanup_module);

MODULE_AUTHOR("Amir Vadai <amir@vadai.me>");
MODULE_DESCRIPTION("ip tunnel manipulation actions");
MODULE_LICENSE("GPL v2");
