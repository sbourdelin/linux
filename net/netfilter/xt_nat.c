/*
 * (C) 1999-2001 Paul `Rusty' Russell
 * (C) 2002-2006 Netfilter Core Team <coreteam@netfilter.org>
 * (C) 2011 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/netfilter.h>
#include <linux/netfilter/x_tables.h>
#include <net/netfilter/nf_nat_core.h>
#include <net/netfilter/nf_nat_l4proto.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>

static void xt_nat_probe_proto(const struct xt_tgchk_param *par)
{
	const struct ip6t_ip6 *i6;
	const struct ipt_ip *i;
	u8 proto, l4proto;

	switch (par->family) {
	case NFPROTO_IPV4:
		i = (const struct ipt_ip *)par->entryinfo;
		if (i->invflags & IPT_INV_PROTO)
			return;
		proto = i->proto;
		break;
	case NFPROTO_IPV6:
		i6 = (const struct ip6t_ip6 *)par->entryinfo;
		if (i6->invflags & IP6T_INV_PROTO)
			return;
		proto = i6->proto;
		break;
	default:
		return;
	}

	switch (proto) {
	case IPPROTO_UDPLITE:
	case IPPROTO_SCTP:
	case IPPROTO_DCCP:
		rcu_read_lock();
		l4proto = __nf_nat_l4proto_find(par->family, proto)->l4proto;
		rcu_read_unlock();
		if (!l4proto)
			request_module("nf-nat-l4-%u", proto);
	}
}

static int xt_nat_checkentry_v0(const struct xt_tgchk_param *par)
{
	const struct nf_nat_ipv4_multi_range_compat *mr = par->targinfo;

	if (mr->rangesize != 1) {
		pr_info("%s: multiple ranges no longer supported\n",
			par->target->name);
		return -EINVAL;
	}
	xt_nat_probe_proto(par);
	return 0;
}

static int xt_nat_checkentry_v1(const struct xt_tgchk_param *par)
{
	xt_nat_probe_proto(par);
	return 0;
}

static void xt_nat_convert_range(struct nf_nat_range *dst,
				 const struct nf_nat_ipv4_range *src)
{
	memset(&dst->min_addr, 0, sizeof(dst->min_addr));
	memset(&dst->max_addr, 0, sizeof(dst->max_addr));

	dst->flags	 = src->flags;
	dst->min_addr.ip = src->min_ip;
	dst->max_addr.ip = src->max_ip;
	dst->min_proto	 = src->min;
	dst->max_proto	 = src->max;
}

static unsigned int
xt_snat_target_v0(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct nf_nat_ipv4_multi_range_compat *mr = par->targinfo;
	struct nf_nat_range range;
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;

	ct = nf_ct_get(skb, &ctinfo);
	NF_CT_ASSERT(ct != NULL &&
		     (ctinfo == IP_CT_NEW || ctinfo == IP_CT_RELATED ||
		      ctinfo == IP_CT_RELATED_REPLY));

	xt_nat_convert_range(&range, &mr->range[0]);
	return nf_nat_setup_info(ct, &range, NF_NAT_MANIP_SRC);
}

static unsigned int
xt_dnat_target_v0(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct nf_nat_ipv4_multi_range_compat *mr = par->targinfo;
	struct nf_nat_range range;
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;

	ct = nf_ct_get(skb, &ctinfo);
	NF_CT_ASSERT(ct != NULL &&
		     (ctinfo == IP_CT_NEW || ctinfo == IP_CT_RELATED));

	xt_nat_convert_range(&range, &mr->range[0]);
	return nf_nat_setup_info(ct, &range, NF_NAT_MANIP_DST);
}

static unsigned int
xt_snat_target_v1(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct nf_nat_range *range = par->targinfo;
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;

	ct = nf_ct_get(skb, &ctinfo);
	NF_CT_ASSERT(ct != NULL &&
		     (ctinfo == IP_CT_NEW || ctinfo == IP_CT_RELATED ||
		      ctinfo == IP_CT_RELATED_REPLY));

	return nf_nat_setup_info(ct, range, NF_NAT_MANIP_SRC);
}

static unsigned int
xt_dnat_target_v1(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct nf_nat_range *range = par->targinfo;
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;

	ct = nf_ct_get(skb, &ctinfo);
	NF_CT_ASSERT(ct != NULL &&
		     (ctinfo == IP_CT_NEW || ctinfo == IP_CT_RELATED));

	return nf_nat_setup_info(ct, range, NF_NAT_MANIP_DST);
}

static struct xt_target xt_nat_target_reg[] __read_mostly = {
	{
		.name		= "SNAT",
		.revision	= 0,
		.checkentry	= xt_nat_checkentry_v0,
		.target		= xt_snat_target_v0,
		.targetsize	= sizeof(struct nf_nat_ipv4_multi_range_compat),
		.family		= NFPROTO_IPV4,
		.table		= "nat",
		.hooks		= (1 << NF_INET_POST_ROUTING) |
				  (1 << NF_INET_LOCAL_IN),
		.me		= THIS_MODULE,
	},
	{
		.name		= "DNAT",
		.revision	= 0,
		.checkentry	= xt_nat_checkentry_v0,
		.target		= xt_dnat_target_v0,
		.targetsize	= sizeof(struct nf_nat_ipv4_multi_range_compat),
		.family		= NFPROTO_IPV4,
		.table		= "nat",
		.hooks		= (1 << NF_INET_PRE_ROUTING) |
				  (1 << NF_INET_LOCAL_OUT),
		.me		= THIS_MODULE,
	},
	{
		.name		= "SNAT",
		.revision	= 1,
		.checkentry     = xt_nat_checkentry_v1,
		.target		= xt_snat_target_v1,
		.targetsize	= sizeof(struct nf_nat_range),
		.table		= "nat",
		.hooks		= (1 << NF_INET_POST_ROUTING) |
				  (1 << NF_INET_LOCAL_IN),
		.me		= THIS_MODULE,
	},
	{
		.name		= "DNAT",
		.revision	= 1,
		.checkentry     = xt_nat_checkentry_v1,
		.target		= xt_dnat_target_v1,
		.targetsize	= sizeof(struct nf_nat_range),
		.table		= "nat",
		.hooks		= (1 << NF_INET_PRE_ROUTING) |
				  (1 << NF_INET_LOCAL_OUT),
		.me		= THIS_MODULE,
	},
};

static int __init xt_nat_init(void)
{
	return xt_register_targets(xt_nat_target_reg,
				   ARRAY_SIZE(xt_nat_target_reg));
}

static void __exit xt_nat_exit(void)
{
	xt_unregister_targets(xt_nat_target_reg, ARRAY_SIZE(xt_nat_target_reg));
}

module_init(xt_nat_init);
module_exit(xt_nat_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Patrick McHardy <kaber@trash.net>");
MODULE_ALIAS("ipt_SNAT");
MODULE_ALIAS("ipt_DNAT");
MODULE_ALIAS("ip6t_SNAT");
MODULE_ALIAS("ip6t_DNAT");
