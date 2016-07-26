/*
 * Copyright (c) 2016 Laura Garcia <nevola@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/smp.h>
#include <linux/static_key.h>
#include <net/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables_core.h>
#include <net/netfilter/nft_nth.h>

void nft_nth_eval(const struct nft_expr *expr,
		  struct nft_regs *regs,
		  const struct nft_pktinfo *pkt)
{
	struct nft_nth *nth = nft_expr_priv(expr);
	u32 nval, oval;

	do {
		oval = atomic_read(&nth->master->counter);
		nval = (oval+1 < nth->every) ? oval+1 : 0;
	} while (atomic_cmpxchg(&nth->master->counter, oval, nval) != oval);

	memcpy(&regs->data[nth->dreg], &nth->master->counter, sizeof(u32));
}
EXPORT_SYMBOL_GPL(nft_nth_eval);

const struct nla_policy nft_nth_policy[NFTA_NTH_MAX + 1] = {
	[NFTA_NTH_DREG]	= { .type = NLA_U32 },
	[NFTA_NTH_DATA]	= { .type = NLA_NESTED },
};

int nft_nth_init(const struct nft_ctx *ctx,
		 const struct nft_expr *expr,
		 const struct nlattr * const tb[])
{
	struct nft_nth *nth = nft_expr_priv(expr);
	struct nft_data_desc desc;
	int err = 0;

	err = nft_data_init(NULL, &nth->data,
			    sizeof(nth->data), &desc,
			    tb[NFTA_NTH_DATA]);
	if (err < 0)
		goto err;

	memcpy(&nth->every, &nth->data, sizeof(u32));
	if (nth->every == 0)
		return -EINVAL;

	nth->dreg = nft_parse_register(tb[NFTA_NTH_DREG]);

	nth->master = kzalloc(sizeof(*nth->master), GFP_KERNEL);
	if (!nth->master)
		return -ENOMEM;
	atomic_set(&nth->master->counter, 0);

err:
	return err;
}
EXPORT_SYMBOL_GPL(nft_nth_init);

int nft_nth_dump(struct sk_buff *skb,
		 const struct nft_expr *expr)
{
	const struct nft_nth *nth = nft_expr_priv(expr);

	if (nft_dump_register(skb, NFTA_NTH_DREG, nth->dreg))
		goto nla_put_failure;
	if (nft_data_dump(skb, NFTA_NTH_DATA, &nth->data,
			  NFT_DATA_VALUE, sizeof(nth->data)) < 0)
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -1;
}
EXPORT_SYMBOL_GPL(nft_nth_dump);

void nft_nth_destroy(const struct nft_ctx *ctx,
		     const struct nft_expr *expr)
{
	struct nft_nth *nth = nft_expr_priv(expr);

	kfree(nth->master);
}
EXPORT_SYMBOL_GPL(nft_nth_destroy);

static struct nft_expr_type nft_nth_type;
static const struct nft_expr_ops nft_nth_ops = {
	.type		= &nft_nth_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_nth)),
	.eval		= nft_nth_eval,
	.init		= nft_nth_init,
	.destroy	= nft_nth_destroy,
	.dump		= nft_nth_dump,
};

static const struct nft_expr_ops *
nft_nth_select_ops(const struct nft_ctx *ctx,
		   const struct nlattr * const tb[])
{
	if (!tb[NFTA_NTH_DREG] ||
	    !tb[NFTA_NTH_DATA])
		return ERR_PTR(-EINVAL);

	return &nft_nth_ops;
}

static struct nft_expr_type nft_nth_type __read_mostly = {
	.name		= "nth",
	.select_ops	= &nft_nth_select_ops,
	.policy		= nft_nth_policy,
	.maxattr	= NFTA_NTH_MAX,
	.owner		= THIS_MODULE,
};

static int __init nft_nth_module_init(void)
{
	return nft_register_expr(&nft_nth_type);
}

static void __exit nft_nth_module_exit(void)
{
	nft_unregister_expr(&nft_nth_type);
}

module_init(nft_nth_module_init);
module_exit(nft_nth_module_exit);

MODULE_LICENSE("GPL");
