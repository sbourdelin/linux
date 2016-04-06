/*
 * Copyright (c) 2016 Pablo Neira Ayuso <pablo@netfilter.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/seqlock.h>
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables.h>

struct nft_nexpr_priv {
	struct nft_nexpr *nexpr;
};

static void nft_nexpr_eval(const struct nft_expr *expr,
			   struct nft_regs *regs,
			   const struct nft_pktinfo *pkt)
{
	struct nft_nexpr_priv *priv = nft_expr_priv(expr);

	priv->nexpr->expr->ops->eval(priv->nexpr->expr, regs, pkt);
}

static int nft_nexpr_dump(struct sk_buff *skb, const struct nft_expr *expr)
{
	struct nft_nexpr_priv *priv = nft_expr_priv(expr);
	const char *type = priv->nexpr->expr->ops->type->name;

	if (nla_put_string(skb, NFTA_NEXPR_REF_NAME, priv->nexpr->name) ||
	    nla_put_string(skb, NFTA_NEXPR_REF_TYPE, type))
		goto nla_put_failure;
	return 0;

nla_put_failure:
	return -1;
}

static const struct nla_policy nft_nexpr_policy[NFTA_NEXPR_REF_MAX + 1] = {
	[NFTA_NEXPR_REF_NAME]	= { .type = NLA_STRING },
	[NFTA_NEXPR_REF_TYPE]	= { .type = NLA_STRING },
};

static int nft_nexpr_init(const struct nft_ctx *ctx,
			  const struct nft_expr *expr,
			  const struct nlattr * const tb[])
{
	struct nft_nexpr_priv *priv = nft_expr_priv(expr);
	struct nft_nexpr *nexpr;

	if (!tb[NFTA_NEXPR_REF_NAME] ||
	    !tb[NFTA_NEXPR_REF_TYPE])
		return -EINVAL;

	nexpr = nft_nexpr_lookup(ctx->table, tb[NFTA_NEXPR_REF_NAME],
				 tb[NFTA_NEXPR_REF_TYPE]);
	if (IS_ERR(nexpr))
		return PTR_ERR(nexpr);

	nexpr->use++;
	priv->nexpr = nexpr;
	return 0;
}

static void nft_nexpr_destroy(const struct nft_ctx *ctx,
			      const struct nft_expr *expr)
{
	struct nft_nexpr_priv *priv = nft_expr_priv(expr);

	priv->nexpr->use--;
}

static struct nft_expr_type nft_nexpr_type;
static const struct nft_expr_ops nft_nexpr_ops = {
	.type		= &nft_nexpr_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_nexpr_priv)),
	.eval		= nft_nexpr_eval,
	.init		= nft_nexpr_init,
	.destroy	= nft_nexpr_destroy,
	.dump		= nft_nexpr_dump,
};

static struct nft_expr_type nft_nexpr_type __read_mostly = {
	.name		= "nexpr",
	.ops		= &nft_nexpr_ops,
	.policy		= nft_nexpr_policy,
	.maxattr	= NFTA_NEXPR_REF_MAX,
	.owner		= THIS_MODULE,
};

static int __init nft_nexpr_module_init(void)
{
	return nft_register_expr(&nft_nexpr_type);
}

static void __exit nft_nexpr_module_exit(void)
{
	nft_unregister_expr(&nft_nexpr_type);
}

module_init(nft_nexpr_module_init);
module_exit(nft_nexpr_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Pablo Neira Ayuso <pablo@netfilter.org");
MODULE_ALIAS_NFT_EXPR("nexpr");
