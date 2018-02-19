/*
 * Copyright (c) 2008-2009 Patrick McHardy <kaber@trash.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Development of this code funded by Astaro AG (http://www.astaro.com/)
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables_core.h>
#include <net/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables_jit.h>

struct nft_cmp_expr {
	struct nft_data		data;
	enum nft_registers	sreg:8;
	u8			len;
	enum nft_cmp_ops	op:8;
};

static void nft_cmp_eval(const struct nft_expr *expr,
			 struct nft_regs *regs,
			 const struct nft_pktinfo *pkt)
{
	const struct nft_cmp_expr *priv = nft_expr_priv(expr);
	int d;

	d = memcmp(&regs->data[priv->sreg], &priv->data, priv->len);
	switch (priv->op) {
	case NFT_CMP_EQ:
		if (d != 0)
			goto mismatch;
		break;
	case NFT_CMP_NEQ:
		if (d == 0)
			goto mismatch;
		break;
	case NFT_CMP_LT:
		if (d == 0)
			goto mismatch;
		/* fall through */
	case NFT_CMP_LTE:
		if (d > 0)
			goto mismatch;
		break;
	case NFT_CMP_GT:
		if (d == 0)
			goto mismatch;
		/* fall through */
	case NFT_CMP_GTE:
		if (d < 0)
			goto mismatch;
		break;
	}
	return;

mismatch:
	regs->verdict.code = NFT_BREAK;
}

static const struct nla_policy nft_cmp_policy[NFTA_CMP_MAX + 1] = {
	[NFTA_CMP_SREG]		= { .type = NLA_U32 },
	[NFTA_CMP_OP]		= { .type = NLA_U32 },
	[NFTA_CMP_DATA]		= { .type = NLA_NESTED },
};

static int nft_cmp_init(const struct nft_ctx *ctx, const struct nft_expr *expr,
			const struct nlattr * const tb[])
{
	struct nft_cmp_expr *priv = nft_expr_priv(expr);
	struct nft_data_desc desc;
	int err;

	err = nft_data_init(NULL, &priv->data, sizeof(priv->data), &desc,
			    tb[NFTA_CMP_DATA]);
	BUG_ON(err < 0);

	priv->sreg = nft_parse_register(tb[NFTA_CMP_SREG]);
	err = nft_validate_register_load(priv->sreg, desc.len);
	if (err < 0)
		return err;

	priv->op  = ntohl(nla_get_be32(tb[NFTA_CMP_OP]));
	priv->len = desc.len;
	return 0;
}

static int nft_cmp_dump(struct sk_buff *skb, const struct nft_expr *expr)
{
	const struct nft_cmp_expr *priv = nft_expr_priv(expr);

	if (nft_dump_register(skb, NFTA_CMP_SREG, priv->sreg))
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_CMP_OP, htonl(priv->op)))
		goto nla_put_failure;

	if (nft_data_dump(skb, NFTA_CMP_DATA, &priv->data,
			  NFT_DATA_VALUE, priv->len) < 0)
		goto nla_put_failure;
	return 0;

nla_put_failure:
	return -1;
}

static enum nft_ast_expr_ops nft_cmp_to_ops[NFT_CMP_GTE + 1] = {
	[NFT_CMP_EQ]	= NFT_AST_OP_EQ,
	[NFT_CMP_NEQ]	= NFT_AST_OP_NEQ,
	[NFT_CMP_LT]	= NFT_AST_OP_LT,
	[NFT_CMP_LTE]	= NFT_AST_OP_LTE,
	[NFT_CMP_GT]	= NFT_AST_OP_GT,
	[NFT_CMP_GTE]	= NFT_AST_OP_GTE,
};

static int nft_ast_expr_cmp_op(enum nft_cmp_ops op)
{
	BUG_ON(op > NFT_CMP_GTE + 1);

	return nft_cmp_to_ops[op];
}

static int __nft_cmp_delinearize(struct nft_ast_expr **regs,
				 const struct nft_cmp_expr *priv,
				 struct list_head *stmt_list)
{
	struct nft_ast_expr *right, *tree;
	struct nft_ast_stmt *stmt;
	int err;

	if (regs[priv->sreg] == NULL)
		return -EINVAL;

	right = nft_ast_expr_alloc(NFT_AST_EXPR_VALUE);
	if (right == NULL)
		return -ENOMEM;

	right->value.data = priv->data;
	right->len = priv->len;

	err = -ENOMEM;
	tree = nft_ast_expr_alloc(NFT_AST_EXPR_RELATIONAL);
	if (tree == NULL)
		goto err1;

	tree->op = nft_ast_expr_cmp_op(priv->op);
	tree->relational.left = regs[priv->sreg];
	tree->relational.right = right;
	tree->len = tree->relational.left->len;

	stmt = nft_ast_stmt_alloc(NFT_AST_STMT_EXPR);
	if (stmt == NULL)
		goto err2;

	stmt->expr = tree;
	list_add_tail(&stmt->list, stmt_list);
	return 0;
err2:
	nft_ast_expr_destroy(tree);
err1:
	nft_ast_expr_destroy(right);
	return err;
}

static int nft_cmp_delinearize(struct nft_ast_expr **regs,
			       const struct nft_expr *expr,
			       struct list_head *stmt_list)
{
	return __nft_cmp_delinearize(regs, nft_expr_priv(expr), stmt_list);
}

static const struct nft_expr_ops nft_cmp_ops = {
	.type		= &nft_cmp_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_cmp_expr)),
	.eval		= nft_cmp_eval,
	.init		= nft_cmp_init,
	.dump		= nft_cmp_dump,
	.delinearize	= nft_cmp_delinearize,
};

static int nft_cmp_fast_init(const struct nft_ctx *ctx,
			     const struct nft_expr *expr,
			     const struct nlattr * const tb[])
{
	struct nft_cmp_fast_expr *priv = nft_expr_priv(expr);
	struct nft_data_desc desc;
	struct nft_data data;
	u32 mask;
	int err;

	err = nft_data_init(NULL, &data, sizeof(data), &desc,
			    tb[NFTA_CMP_DATA]);
	BUG_ON(err < 0);

	priv->sreg = nft_parse_register(tb[NFTA_CMP_SREG]);
	err = nft_validate_register_load(priv->sreg, desc.len);
	if (err < 0)
		return err;

	desc.len *= BITS_PER_BYTE;
	mask = nft_cmp_fast_mask(desc.len);

	priv->data = data.data[0] & mask;
	priv->len  = desc.len;
	return 0;
}

static int nft_cmp_fast_dump(struct sk_buff *skb, const struct nft_expr *expr)
{
	const struct nft_cmp_fast_expr *priv = nft_expr_priv(expr);
	struct nft_data data;

	if (nft_dump_register(skb, NFTA_CMP_SREG, priv->sreg))
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_CMP_OP, htonl(NFT_CMP_EQ)))
		goto nla_put_failure;

	data.data[0] = priv->data;
	if (nft_data_dump(skb, NFTA_CMP_DATA, &data,
			  NFT_DATA_VALUE, priv->len / BITS_PER_BYTE) < 0)
		goto nla_put_failure;
	return 0;

nla_put_failure:
	return -1;
}

static int nft_cmp_fast_delinearize(struct nft_ast_expr **regs,
				    const struct nft_expr *expr,
				    struct list_head *stmt_list)
{
	const struct nft_cmp_fast_expr *priv = nft_expr_priv(expr);
	struct nft_cmp_expr cmp = {
		.data   = {
			.data   = {
				[0] = priv->data,
			},
		},
		.sreg   = priv->sreg,
		.len    = priv->len,
		.op     = NFT_AST_OP_EQ,
	};

	return __nft_cmp_delinearize(regs, &cmp, stmt_list);
}

const struct nft_expr_ops nft_cmp_fast_ops = {
	.type		= &nft_cmp_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_cmp_fast_expr)),
	.eval		= NULL,	/* inlined */
	.init		= nft_cmp_fast_init,
	.dump		= nft_cmp_fast_dump,
	.delinearize	= nft_cmp_fast_delinearize,
};

static const struct nft_expr_ops *
nft_cmp_select_ops(const struct nft_ctx *ctx, const struct nlattr * const tb[])
{
	struct nft_data_desc desc;
	struct nft_data data;
	enum nft_cmp_ops op;
	int err;

	if (tb[NFTA_CMP_SREG] == NULL ||
	    tb[NFTA_CMP_OP] == NULL ||
	    tb[NFTA_CMP_DATA] == NULL)
		return ERR_PTR(-EINVAL);

	op = ntohl(nla_get_be32(tb[NFTA_CMP_OP]));
	switch (op) {
	case NFT_CMP_EQ:
	case NFT_CMP_NEQ:
	case NFT_CMP_LT:
	case NFT_CMP_LTE:
	case NFT_CMP_GT:
	case NFT_CMP_GTE:
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	err = nft_data_init(NULL, &data, sizeof(data), &desc,
			    tb[NFTA_CMP_DATA]);
	if (err < 0)
		return ERR_PTR(err);

	if (desc.type != NFT_DATA_VALUE) {
		err = -EINVAL;
		goto err1;
	}

	if (desc.len <= sizeof(u32) && op == NFT_CMP_EQ)
		return &nft_cmp_fast_ops;

	return &nft_cmp_ops;
err1:
	nft_data_release(&data, desc.type);
	return ERR_PTR(-EINVAL);
}

struct nft_expr_type nft_cmp_type __read_mostly = {
	.name		= "cmp",
	.select_ops	= nft_cmp_select_ops,
	.policy		= nft_cmp_policy,
	.maxattr	= NFTA_CMP_MAX,
	.owner		= THIS_MODULE,
};
