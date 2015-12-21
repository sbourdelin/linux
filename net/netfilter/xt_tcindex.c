/*
 *	xt_tcindex - Netfilter module to match/tag on tc_index mark value
 *
 *	(C) 2015 Allied Telesis Labs NZ.
  *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU General Public License version 2 as
 *	published by the Free Software Foundation.
 *
 *	Heavily based on xt_mark.c
 */

#include <linux/module.h>
#include <linux/skbuff.h>

#include <linux/netfilter/xt_tcindex.h>
#include <linux/netfilter/x_tables.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Luuk Paulussen <luuk.paulussen@alliedtelesis.co.nz>");
MODULE_DESCRIPTION("Xtables: packet tc_index mark operations");
MODULE_ALIAS("ipt_tcindex");
MODULE_ALIAS("ip6t_tcindex");
MODULE_ALIAS("ipt_TCINDEX");
MODULE_ALIAS("ip6t_TCINDEX");

static unsigned int
tcindex_tg(struct sk_buff *skb, const struct xt_action_param *par)
{
	const struct xt_tcindex_tginfo1 *info = par->targinfo;

	skb->tc_index = (skb->tc_index & ~info->mask) ^ info->mark;
	return XT_CONTINUE;
}

static bool
tcindex_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	const struct xt_tcindex_mtinfo1 *info = par->matchinfo;

	return ((skb->tc_index & info->mask) == info->mark) ^ info->invert;
}

static struct xt_target tcindex_tg_reg __read_mostly = {
	.name           = "TCINDEX",
	.revision       = 1,
	.family         = NFPROTO_UNSPEC,
	.target         = tcindex_tg,
	.targetsize     = sizeof(struct xt_tcindex_tginfo1),
	.me             = THIS_MODULE,
};

static struct xt_match tcindex_mt_reg __read_mostly = {
	.name           = "tcindex",
	.revision       = 1,
	.family         = NFPROTO_UNSPEC,
	.match          = tcindex_mt,
	.matchsize      = sizeof(struct xt_tcindex_mtinfo1),
	.me             = THIS_MODULE,
};

static int __init tcindex_mt_init(void)
{
	int ret;

	ret = xt_register_target(&tcindex_tg_reg);
	if (ret < 0)
		return ret;
	ret = xt_register_match(&tcindex_mt_reg);
	if (ret < 0) {
		xt_unregister_target(&tcindex_tg_reg);
		return ret;
	}
	return 0;
}

static void __exit tcindex_mt_exit(void)
{
	xt_unregister_match(&tcindex_mt_reg);
	xt_unregister_target(&tcindex_tg_reg);
}

module_init(tcindex_mt_init);
module_exit(tcindex_mt_exit);
