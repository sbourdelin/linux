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
#include <net/netfilter/nf_tables.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include <net/netfilter/nf_conntrack_tuple.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_conntrack_ecache.h>
#include <net/netfilter/nf_conntrack_labels.h>

struct nft_ct {
	enum nft_ct_keys	key:8;
	enum ip_conntrack_dir	dir:8;
	union {
		enum nft_registers	dreg:8;
		enum nft_registers	sreg:8;
	};
	union {
		u8		set_bit;
	} imm;
	unsigned int		imm_len:8;
	struct nft_data		immediate;
};

static u64 nft_ct_get_eval_counter(const struct nf_conn_counter *c,
				   enum nft_ct_keys k,
				   enum ip_conntrack_dir d)
{
	if (d < IP_CT_DIR_MAX)
		return k == NFT_CT_BYTES ? atomic64_read(&c[d].bytes) :
					   atomic64_read(&c[d].packets);

	return nft_ct_get_eval_counter(c, k, IP_CT_DIR_ORIGINAL) +
	       nft_ct_get_eval_counter(c, k, IP_CT_DIR_REPLY);
}

static void nft_ct_get_eval(const struct nft_expr *expr,
			    struct nft_regs *regs,
			    const struct nft_pktinfo *pkt)
{
	const struct nft_ct *priv = nft_expr_priv(expr);
	u32 *dest = &regs->data[priv->dreg];
	enum ip_conntrack_info ctinfo;
	const struct nf_conn *ct;
	const struct nf_conn_help *help;
	const struct nf_conntrack_tuple *tuple;
	const struct nf_conntrack_helper *helper;
	long diff;
	unsigned int state;

	ct = nf_ct_get(pkt->skb, &ctinfo);

	switch (priv->key) {
	case NFT_CT_STATE:
		if (ct == NULL)
			state = NF_CT_STATE_INVALID_BIT;
		else if (nf_ct_is_untracked(ct))
			state = NF_CT_STATE_UNTRACKED_BIT;
		else
			state = NF_CT_STATE_BIT(ctinfo);
		*dest = state;
		return;
	default:
		break;
	}

	if (ct == NULL)
		goto err;

	switch (priv->key) {
	case NFT_CT_DIRECTION:
		*dest = CTINFO2DIR(ctinfo);
		return;
	case NFT_CT_STATUS:
		*dest = ct->status;
		return;
#ifdef CONFIG_NF_CONNTRACK_MARK
	case NFT_CT_MARK:
		*dest = ct->mark;
		return;
#endif
#ifdef CONFIG_NF_CONNTRACK_SECMARK
	case NFT_CT_SECMARK:
		*dest = ct->secmark;
		return;
#endif
	case NFT_CT_EXPIRATION:
		diff = (long)jiffies - (long)ct->timeout.expires;
		if (diff < 0)
			diff = 0;
		*dest = jiffies_to_msecs(diff);
		return;
	case NFT_CT_HELPER:
		if (ct->master == NULL)
			goto err;
		help = nfct_help(ct->master);
		if (help == NULL)
			goto err;
		helper = rcu_dereference(help->helper);
		if (helper == NULL)
			goto err;
		strncpy((char *)dest, helper->name, NF_CT_HELPER_NAME_LEN);
		return;
#ifdef CONFIG_NF_CONNTRACK_LABELS
	case NFT_CT_LABELS: {
		struct nf_conn_labels *labels = nf_ct_labels_find(ct);
		unsigned int size;

		if (!labels) {
			memset(dest, 0, NF_CT_LABELS_MAX_SIZE);
			return;
		}

		size = labels->words * sizeof(long);
		memcpy(dest, labels->bits, size);
		if (size < NF_CT_LABELS_MAX_SIZE)
			memset(((char *) dest) + size, 0,
			       NF_CT_LABELS_MAX_SIZE - size);
		return;
	}
#endif
	case NFT_CT_BYTES: /* fallthrough */
	case NFT_CT_PKTS: {
		const struct nf_conn_acct *acct = nf_conn_acct_find(ct);
		u64 count = 0;

		if (acct)
			count = nft_ct_get_eval_counter(acct->counter,
							priv->key, priv->dir);
		memcpy(dest, &count, sizeof(count));
		return;
	}
	default:
		break;
	}

	tuple = &ct->tuplehash[priv->dir].tuple;
	switch (priv->key) {
	case NFT_CT_L3PROTOCOL:
		*dest = nf_ct_l3num(ct);
		return;
	case NFT_CT_SRC:
		memcpy(dest, tuple->src.u3.all,
		       nf_ct_l3num(ct) == NFPROTO_IPV4 ? 4 : 16);
		return;
	case NFT_CT_DST:
		memcpy(dest, tuple->dst.u3.all,
		       nf_ct_l3num(ct) == NFPROTO_IPV4 ? 4 : 16);
		return;
	case NFT_CT_PROTOCOL:
		*dest = nf_ct_protonum(ct);
		return;
	case NFT_CT_PROTO_SRC:
		*dest = (__force __u16)tuple->src.u.all;
		return;
	case NFT_CT_PROTO_DST:
		*dest = (__force __u16)tuple->dst.u.all;
		return;
	default:
		break;
	}
	return;
err:
	regs->verdict.code = NFT_BREAK;
}

static void nft_ct_set_eval(const struct nft_expr *expr,
			    struct nft_regs *regs,
			    const struct nft_pktinfo *pkt)
{
	const struct nft_ct *priv = nft_expr_priv(expr);
	struct sk_buff *skb = pkt->skb;
#ifdef CONFIG_NF_CONNTRACK_MARK
	u32 value = regs->data[priv->sreg];
#endif
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct;

	ct = nf_ct_get(skb, &ctinfo);
	if (ct == NULL)
		return;

	switch (priv->key) {
#ifdef CONFIG_NF_CONNTRACK_MARK
	case NFT_CT_MARK:
		if (ct->mark != value) {
			ct->mark = value;
			nf_conntrack_event_cache(IPCT_MARK, ct);
		}
		break;
#endif
#ifdef CONFIG_NF_CONNTRACK_LABELS
	case NFT_CT_LABELS:
		nf_connlabel_set(ct, priv->imm.set_bit);
		break;
#endif
	default:
		break;
	}
}

static const struct nla_policy nft_ct_policy[NFTA_CT_MAX + 1] = {
	[NFTA_CT_DREG]		= { .type = NLA_U32 },
	[NFTA_CT_KEY]		= { .type = NLA_U32 },
	[NFTA_CT_DIRECTION]	= { .type = NLA_U8 },
	[NFTA_CT_SREG]		= { .type = NLA_U32 },
	[NFTA_CT_IMM]		= { .type = NLA_NESTED },
};

static int nft_ct_l3proto_try_module_get(uint8_t family)
{
	int err;

	if (family == NFPROTO_INET) {
		err = nf_ct_l3proto_try_module_get(NFPROTO_IPV4);
		if (err < 0)
			goto err1;
		err = nf_ct_l3proto_try_module_get(NFPROTO_IPV6);
		if (err < 0)
			goto err2;
	} else {
		err = nf_ct_l3proto_try_module_get(family);
		if (err < 0)
			goto err1;
	}
	return 0;

err2:
	nf_ct_l3proto_module_put(NFPROTO_IPV4);
err1:
	return err;
}

static void nft_ct_l3proto_module_put(uint8_t family)
{
	if (family == NFPROTO_INET) {
		nf_ct_l3proto_module_put(NFPROTO_IPV4);
		nf_ct_l3proto_module_put(NFPROTO_IPV6);
	} else
		nf_ct_l3proto_module_put(family);
}

static int nft_ct_get_init(const struct nft_ctx *ctx,
			   const struct nft_expr *expr,
			   const struct nlattr * const tb[])
{
	struct nft_ct *priv = nft_expr_priv(expr);
	unsigned int len;
	int err;

	priv->key = ntohl(nla_get_be32(tb[NFTA_CT_KEY]));
	switch (priv->key) {
	case NFT_CT_DIRECTION:
		if (tb[NFTA_CT_DIRECTION] != NULL)
			return -EINVAL;
		len = sizeof(u8);
		break;
	case NFT_CT_STATE:
	case NFT_CT_STATUS:
#ifdef CONFIG_NF_CONNTRACK_MARK
	case NFT_CT_MARK:
#endif
#ifdef CONFIG_NF_CONNTRACK_SECMARK
	case NFT_CT_SECMARK:
#endif
	case NFT_CT_EXPIRATION:
		if (tb[NFTA_CT_DIRECTION] != NULL)
			return -EINVAL;
		len = sizeof(u32);
		break;
#ifdef CONFIG_NF_CONNTRACK_LABELS
	case NFT_CT_LABELS:
		if (tb[NFTA_CT_DIRECTION] != NULL)
			return -EINVAL;
		len = NF_CT_LABELS_MAX_SIZE;
		err = nf_connlabels_get(ctx->net, (len * BITS_PER_BYTE) - 1);
		if (err)
			return err;
		break;
#endif
	case NFT_CT_HELPER:
		if (tb[NFTA_CT_DIRECTION] != NULL)
			return -EINVAL;
		len = NF_CT_HELPER_NAME_LEN;
		break;

	case NFT_CT_L3PROTOCOL:
	case NFT_CT_PROTOCOL:
		if (tb[NFTA_CT_DIRECTION] == NULL)
			return -EINVAL;
		len = sizeof(u8);
		break;
	case NFT_CT_SRC:
	case NFT_CT_DST:
		if (tb[NFTA_CT_DIRECTION] == NULL)
			return -EINVAL;

		switch (ctx->afi->family) {
		case NFPROTO_IPV4:
			len = FIELD_SIZEOF(struct nf_conntrack_tuple,
					   src.u3.ip);
			break;
		case NFPROTO_IPV6:
		case NFPROTO_INET:
			len = FIELD_SIZEOF(struct nf_conntrack_tuple,
					   src.u3.ip6);
			break;
		default:
			return -EAFNOSUPPORT;
		}
		break;
	case NFT_CT_PROTO_SRC:
	case NFT_CT_PROTO_DST:
		if (tb[NFTA_CT_DIRECTION] == NULL)
			return -EINVAL;
		len = FIELD_SIZEOF(struct nf_conntrack_tuple, src.u.all);
		break;
	case NFT_CT_BYTES:
	case NFT_CT_PKTS:
		/* no direction? return sum of original + reply */
		if (tb[NFTA_CT_DIRECTION] == NULL)
			priv->dir = IP_CT_DIR_MAX;
		len = sizeof(u64);
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (tb[NFTA_CT_DIRECTION] != NULL) {
		priv->dir = nla_get_u8(tb[NFTA_CT_DIRECTION]);
		switch (priv->dir) {
		case IP_CT_DIR_ORIGINAL:
		case IP_CT_DIR_REPLY:
			break;
		default:
			return -EINVAL;
		}
	}

	priv->dreg = nft_parse_register(tb[NFTA_CT_DREG]);
	err = nft_validate_register_store(ctx, priv->dreg, NULL,
					  NFT_DATA_VALUE, len);
	if (err < 0)
		return err;

	err = nft_ct_l3proto_try_module_get(ctx->afi->family);
	if (err < 0)
		return err;

	return 0;
}

static int nft_ct_set_init(const struct nft_ctx *ctx,
			   const struct nft_expr *expr,
			   const struct nlattr * const tb[])
{
	struct nft_ct *priv = nft_expr_priv(expr);
	struct nft_data_desc imm_desc = {};
	unsigned int len;
	int err;

	priv->key = ntohl(nla_get_be32(tb[NFTA_CT_KEY]));

	if (tb[NFTA_CT_IMM]) {
		/* We currently do not support both sreg and imm */
		if (tb[NFTA_CT_SREG])
			return -EINVAL;

		err = nft_data_init(NULL, &priv->immediate, sizeof(priv->immediate),
				    &imm_desc, tb[NFTA_CT_IMM]);
		if (err < 0)
			return err;

		if (imm_desc.type != NFT_DATA_VALUE)
			return -EINVAL;
	}
	switch (priv->key) {
#ifdef CONFIG_NF_CONNTRACK_MARK
	case NFT_CT_MARK:
		if (tb[NFTA_CT_DIRECTION])
			return -EINVAL;
		len = FIELD_SIZEOF(struct nf_conn, mark);
		break;
#endif
#ifdef CONFIG_NF_CONNTRACK_LABELS
	case NFT_CT_LABELS:
		if (tb[NFTA_CT_DIRECTION] || imm_desc.len != sizeof(u32))
			return -EINVAL;

		err = nf_connlabels_get(ctx->net, htonl(priv->immediate.data[0]));
		if (err < 0)
			return err;

		priv->imm_len = sizeof(u32);
		priv->imm.set_bit = htonl(priv->immediate.data[0]);

		err = nft_ct_l3proto_try_module_get(ctx->afi->family);
		if (err < 0)
			nf_connlabels_put(ctx->net);
		return err;
#endif
	default:
		return -EOPNOTSUPP;
	}

	priv->sreg = nft_parse_register(tb[NFTA_CT_SREG]);
	err = nft_validate_register_load(priv->sreg, len);
	if (err < 0)
		return err;

	err = nft_ct_l3proto_try_module_get(ctx->afi->family);
	if (err < 0)
		return err;

	return 0;
}

static void nft_ct_destroy(const struct nft_ctx *ctx,
			   const struct nft_expr *expr)
{
	struct nft_ct *priv = nft_expr_priv(expr);

	switch (priv->key) {
#ifdef CONFIG_NF_CONNTRACK_LABELS
	case NFT_CT_LABELS:
		nf_connlabels_put(ctx->net);
		break;
#endif
	default:
		break;
	}

	nft_ct_l3proto_module_put(ctx->afi->family);
}

static int nft_ct_get_dump(struct sk_buff *skb, const struct nft_expr *expr)
{
	const struct nft_ct *priv = nft_expr_priv(expr);

	if (nft_dump_register(skb, NFTA_CT_DREG, priv->dreg))
		goto nla_put_failure;
	if (nla_put_be32(skb, NFTA_CT_KEY, htonl(priv->key)))
		goto nla_put_failure;

	switch (priv->key) {
	case NFT_CT_L3PROTOCOL:
	case NFT_CT_PROTOCOL:
	case NFT_CT_SRC:
	case NFT_CT_DST:
	case NFT_CT_PROTO_SRC:
	case NFT_CT_PROTO_DST:
		if (nla_put_u8(skb, NFTA_CT_DIRECTION, priv->dir))
			goto nla_put_failure;
		break;
	case NFT_CT_BYTES:
	case NFT_CT_PKTS:
		if (priv->dir < IP_CT_DIR_MAX &&
		    nla_put_u8(skb, NFTA_CT_DIRECTION, priv->dir))
			goto nla_put_failure;
		break;
	default:
		break;
	}

	return 0;

nla_put_failure:
	return -1;
}

static int nft_ct_set_dump(struct sk_buff *skb, const struct nft_expr *expr)
{
	const struct nft_ct *priv = nft_expr_priv(expr);

	if (nla_put_be32(skb, NFTA_CT_KEY, htonl(priv->key)))
		goto nla_put_failure;

	if (priv->imm_len) {
		if (nft_data_dump(skb, NFTA_CT_IMM, &priv->immediate,
				  NFT_DATA_VALUE, priv->imm_len) < 0)
			goto nla_put_failure;

		return 0;
	}

	if (nft_dump_register(skb, NFTA_CT_SREG, priv->sreg))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -1;
}

static struct nft_expr_type nft_ct_type;
static const struct nft_expr_ops nft_ct_get_ops = {
	.type		= &nft_ct_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_ct)),
	.eval		= nft_ct_get_eval,
	.init		= nft_ct_get_init,
	.destroy	= nft_ct_destroy,
	.dump		= nft_ct_get_dump,
};

static const struct nft_expr_ops nft_ct_set_ops = {
	.type		= &nft_ct_type,
	.size		= NFT_EXPR_SIZE(sizeof(struct nft_ct)),
	.eval		= nft_ct_set_eval,
	.init		= nft_ct_set_init,
	.destroy	= nft_ct_destroy,
	.dump		= nft_ct_set_dump,
};

static const struct nft_expr_ops *
nft_ct_select_ops(const struct nft_ctx *ctx,
		    const struct nlattr * const tb[])
{
	if (tb[NFTA_CT_KEY] == NULL)
		return ERR_PTR(-EINVAL);

	if (tb[NFTA_CT_DREG] && tb[NFTA_CT_SREG])
		return ERR_PTR(-EINVAL);

	if (tb[NFTA_CT_DREG])
		return &nft_ct_get_ops;

	if (tb[NFTA_CT_SREG] || tb[NFTA_CT_IMM])
		return &nft_ct_set_ops;

	return ERR_PTR(-EINVAL);
}

static struct nft_expr_type nft_ct_type __read_mostly = {
	.name		= "ct",
	.select_ops	= &nft_ct_select_ops,
	.policy		= nft_ct_policy,
	.maxattr	= NFTA_CT_MAX,
	.owner		= THIS_MODULE,
};

static int __init nft_ct_module_init(void)
{
	BUILD_BUG_ON(NF_CT_LABELS_MAX_SIZE > NFT_REG_SIZE);

	return nft_register_expr(&nft_ct_type);
}

static void __exit nft_ct_module_exit(void)
{
	nft_unregister_expr(&nft_ct_type);
}

module_init(nft_ct_module_init);
module_exit(nft_ct_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Patrick McHardy <kaber@trash.net>");
MODULE_ALIAS_NFT_EXPR("ct");
