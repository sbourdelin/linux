#include <net/netfilter/nf_tables.h>
#include <linux/netfilter/nf_osf.h>

#define OSF_GENRE_SIZE 32

struct nft_osf {
	char	genre[OSF_GENRE_SIZE];
	__u32	flags;
	__u32	loglevel;
	__u32	ttl;
	__u32	len;
};

/* placeholder function WIP */
static inline bool match_packet(struct nft_osf *priv, struct sk_buff *skb)
{
	return 1;
}

static const struct nla_policy nft_osf_policy[NFTA_OSF_MAX + 1] = {
	[NFTA_OSF_GENRE]	= { .type = NLA_STRING, .len = OSF_GENRE_SIZE },
	[NFTA_OSF_FLAGS]	= { .type = NLA_U32 },
	[NFTA_OSF_LOGLEVEL]	= { .type = NLA_U32 },
	[NFTA_OSF_TTL]		= { .type = NLA_U32 },
};

static void nft_osf_eval(const struct nft_expr *expr, struct nft_regs *regs,
			 const struct nft_pktinfo *pkt)
{
	struct nft_osf *priv = nft_expr_priv(expr);
	struct sk_buff *skb = pkt->skb;

	if (!match_packet(priv, skb))
		regs->verdict.code = NFT_BREAK;
}

static int nft_osf_init(const struct nft_ctx *ctx,
			const struct nft_expr *expr,
			const struct nlattr * const tb[])
{
	struct nft_osf *priv = nft_expr_priv(expr);

	if (tb[NFTA_OSF_GENRE] == NULL)
		return -EINVAL;
	nla_strlcpy(priv->genre, tb[NFTA_OSF_GENRE], OSF_GENRE_SIZE);
	if (ntohl(nla_get_be32(tb[NFTA_OSF_FLAGS])) & ~NF_OSF_FLAGMASK)
		return -EINVAL;
	priv->flags	= ntohl(nla_get_be32(tb[NFTA_OSF_FLAGS]));
	if (ntohl(nla_get_be32(tb[NFTA_OSF_LOGLEVEL])) >=
			       NF_OSF_LOGLEVEL_ALL_KNOWN)
		return -EINVAL;
	priv->loglevel	= ntohl(nla_get_be32(tb[NFTA_OSF_LOGLEVEL]));
	if (ntohl(nla_get_be32(tb[NFTA_OSF_TTL])) >= NF_OSF_TTL_NOCHECK)
		return -EINVAL;
	priv->ttl	= ntohl(nla_get_be32(tb[NFTA_OSF_TTL]));
	priv->len = strlen(priv->genre);
	return 0;
}

static int nft_osf_dump(struct sk_buff *skb, const struct nft_expr *expr)
{
	const struct nft_osf *priv = nft_expr_priv(expr);

	if (nla_put_string(skb, NFTA_OSF_GENRE, priv->genre) ||
	    nla_put_be32(skb, NFTA_OSF_FLAGS, htonl(priv->flags)) ||
	    nla_put_be32(skb, NFTA_OSF_LOGLEVEL, htonl(priv->loglevel)) ||
	    nla_put_be32(skb, NFTA_OSF_TTL, htonl(priv->ttl)))
		goto nla_put_failure;
	return 0;

nla_put_failure:
	return -1;
}

static struct nft_expr_type nft_osf_type;

static const struct nft_expr_ops nft_osf_op = {
	.eval = nft_osf_eval,
	.size = NFT_EXPR_SIZE(sizeof(struct nft_osf)),
	.init = nft_osf_init,
	.dump = nft_osf_dump,
	.type = &nft_osf_type,
};

static struct nft_expr_type nft_osf_type __read_mostly = {
	.ops = &nft_osf_op,
	.name = "osf",
	.owner = THIS_MODULE,
	.policy = nft_osf_policy,
	.maxattr = NFTA_OSF_MAX,
};

static int __init nft_osf_module_init(void)
{
	return nft_register_expr(&nft_osf_type);
}

static void __exit nft_osf_module_exit(void)
{
	return nft_unregister_expr(&nft_osf_type);
}

module_init(nft_osf_module_init);
module_exit(nft_osf_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Fernando Fernandez <ffmancera@riseup.net>");
MODULE_ALIAS_NFT_EXPR("osf");
