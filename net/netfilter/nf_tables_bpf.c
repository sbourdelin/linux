#include <linux/types.h>
#include <linux/filter.h>
#include <net/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables_jit.h>

#define NFT_EMIT(jit, __insn)					\
	jit->insn[jit->len] = (struct sock_filter) __insn;	\
	jit->len++;						\

static int nft_jit_bpf_payload_xfrm(const struct nft_ast_expr *expr,
				    struct nft_ast_xfrm_state *state,
				    void *data)
{
	struct nft_ast_expr *right = expr->relational.right;
	struct nft_ast_expr *left = expr->relational.left;
	struct nft_rule_jit *jit = data;
	unsigned int size;
	unsigned int k;

	pr_info("> match payload at offset %u base %u len %u\n",
		left->payload.offset, left->payload.base, left->len);

	switch (left->len) {
	case 1:
		size = BPF_B;
		k = right->value.data.data[0];
		break;
	case 2:
		size = BPF_H;
		k = ntohs(right->value.data.data[0]);
		break;
	case 4:
		size = BPF_W;
		k = ntohl(right->value.data.data[0]);
		break;
	default:
		return -EOPNOTSUPP;
	}

	NFT_EMIT(jit, BPF_STMT(BPF_LD | size | BPF_ABS, left->payload.offset));
	NFT_EMIT(jit, BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, k, 0, 1));
	NFT_EMIT(jit, BPF_STMT(BPF_RET | BPF_K, NF_DROP));
	NFT_EMIT(jit, BPF_STMT(BPF_RET | BPF_K, NF_ACCEPT));

	return 0;
}

static const struct nft_ast_proto_desc nft_jit_bpf_payload_desc = {
	.xfrm		= nft_jit_bpf_payload_xfrm,
};

static int nft_jit_bpf_meta_xfrm(const struct nft_ast_expr *expr,
				 struct nft_ast_xfrm_state *state, void *data)
{
	struct nft_ast_expr *right = expr->relational.right;
	struct nft_ast_expr *left = expr->relational.left;
	struct nft_rule_jit *jit = data;
	unsigned int ad;

	pr_info("> generate meta match\n");

	switch (left->meta.key) {
	case NFT_META_PROTOCOL:
		pr_info("meta protocol\n");
		ad = SKF_AD_PROTOCOL;
		break;
		break;
	case NFT_META_IIF:
		pr_info("meta iif\n");
		ad = SKF_AD_IFINDEX;
		break;
	default:
		return -EOPNOTSUPP;
	}

	NFT_EMIT(jit, BPF_STMT(BPF_LD | BPF_W | BPF_ABS, SKF_AD_OFF + ad));
	NFT_EMIT(jit, BPF_JUMP(BPF_JMP | BPF_JEQ,
			       right->value.data.data[0], 0, 1));
	NFT_EMIT(jit, BPF_STMT(BPF_RET | BPF_K, NF_DROP));
	NFT_EMIT(jit, BPF_STMT(BPF_RET | BPF_K, NF_ACCEPT));

	return 0;
}

static const struct nft_ast_meta_desc nft_jit_bpf_meta_desc = {
	.xfrm		= nft_jit_bpf_meta_xfrm,
};

struct nft_ast_xfrm_desc nft_jit_bpf_xfrm_desc = {
	.proto_desc	= &nft_jit_bpf_payload_desc,
	.meta_desc	= &nft_jit_bpf_meta_desc,
};
