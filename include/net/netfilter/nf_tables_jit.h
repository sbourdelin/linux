#ifndef _NFTABLES_JIT_H_
#define _NFTABLES_JIT_H_

#include <uapi/linux/netfilter/nf_tables.h>

enum nft_ast_expr_type {
	NFT_AST_EXPR_UNSPEC	= 0,
	NFT_AST_EXPR_RELATIONAL,
	NFT_AST_EXPR_VALUE,
	NFT_AST_EXPR_META,
	NFT_AST_EXPR_PAYLOAD,
};

enum nft_ast_expr_ops {
	NFT_AST_OP_INVALID,
	NFT_AST_OP_EQ,
	NFT_AST_OP_NEQ,
	NFT_AST_OP_LT,
	NFT_AST_OP_LTE,
	NFT_AST_OP_GT,
	NFT_AST_OP_GTE,
	NFT_AST_OP_AND,
	NFT_AST_OP_OR,
	NFT_AST_OP_XOR,
};

/**
 *	struct nft_ast_expr - nf_tables delinearized expression
 *
 *	@type: expression type
 *	@op: type of operation
 *	@len: length of expression
 */
struct nft_ast_expr {
	enum nft_ast_expr_type		type;
	enum nft_ast_expr_ops		op;
	u32				len;
	union {
		struct {
			struct nft_data		data;
		} value;
		struct {
			enum nft_meta_keys	key;
		} meta;
		struct {
			enum nft_payload_bases	base;
			u32			offset;
		} payload;
		struct {
			struct nft_ast_expr	*left;
			struct nft_ast_expr	*right;
		} relational;
	};
};

struct nft_ast_expr *nft_ast_expr_alloc(enum nft_ast_expr_type type);
void nft_ast_expr_destroy(struct nft_ast_expr *expr);

enum nft_ast_stmt_type {
	NFT_AST_STMT_EXPR		= 0,
};

/**
 *	struct nft_ast_stmt - nf_tables delinearized statement
 *
 *	@type: statement type
 */
struct nft_ast_stmt {
	struct list_head			list;

	enum nft_ast_stmt_type			type;
	union {
		struct nft_ast_expr		*expr;
	};
};

struct nft_ast_stmt *nft_ast_stmt_alloc(enum nft_ast_stmt_type type);
void nft_ast_stmt_list_release(struct list_head *ast_stmt_list);

void nft_ast_stmt_list_print(struct list_head *stmt_list);

int nft_delinearize(struct list_head *ast_stmt_list, struct nft_rule *rule);

/*
 * Tree of transformation callback definitions.
 */
struct nft_ast_xfrm_state;

/**
 *	struct nft_ast_proto_desc - nf_tables protocol transformation description
 *
 *	@xfrm: transformation callback
 */
struct nft_ast_proto_desc {
	int (*xfrm)(const struct nft_ast_expr *dlexpr,
		    struct nft_ast_xfrm_state *state, void *data);
};

/**
 *	struct nft_ast_meta_desc - nf_tables meta transformation description
 *
 *	@xfrm: transformation callback
 */
struct nft_ast_meta_desc {
	int (*xfrm)(const struct nft_ast_expr *dlexpr,
		    struct nft_ast_xfrm_state *state, void *data);
};

/**
 *	struct nft_ast_xfrm_desc - nf_tables generic transformation description
 *
 *	@key: meta key
 *	@xfrm: transformation callback
 */
struct nft_ast_xfrm_desc {
	const struct nft_ast_proto_desc	*proto_desc;
	const struct nft_ast_meta_desc	*meta_desc;
};

int nft_ast_xfrm(const struct list_head *ast_stmt_list,
		 const struct nft_ast_xfrm_desc *base_desc, void *data);

#endif
