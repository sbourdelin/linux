#ifndef _NFT_NTH_H_
#define _NFT_NTH_H_

struct nft_nth {
	enum nft_registers      dreg:8;
	struct nft_data         data;
	u32		every;
	struct nft_nth_priv *master __attribute__((aligned(8)));
};

struct nft_nth_priv {
	atomic_t counter;
} ____cacheline_aligned_in_smp;

extern const struct nla_policy nft_nth_policy[];

int nft_nth_init(const struct nft_ctx *ctx,
		 const struct nft_expr *expr,
		 const struct nlattr * const tb[]);

int nft_nth_dump(struct sk_buff *skb,
		 const struct nft_expr *expr);

void nft_nth_eval(const struct nft_expr *expr,
		  struct nft_regs *regs,
		  const struct nft_pktinfo *pkt);

void nft_nth_destroy(const struct nft_ctx *ctx,
		     const struct nft_expr *expr);

#endif
