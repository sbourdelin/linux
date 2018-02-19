#include <linux/init.h>
#include <linux/module.h>
#include <linux/netfilter.h>
#include <net/netfilter/nf_tables.h>
#include <net/netfilter/nf_tables_jit.h>

struct nft_ast_expr *nft_ast_expr_alloc(enum nft_ast_expr_type type)
{
	struct nft_ast_expr *expr;

	expr = kmalloc(sizeof(struct nft_ast_expr), GFP_KERNEL);
	if (expr == NULL)
		return NULL;

	expr->type = type;
	expr->op   = NFT_AST_OP_INVALID;

	return expr;
}
EXPORT_SYMBOL_GPL(nft_ast_expr_alloc);

void nft_ast_expr_destroy(struct nft_ast_expr *expr)
{
	switch (expr->type) {
	case NFT_AST_EXPR_VALUE:
	case NFT_AST_EXPR_META:
	case NFT_AST_EXPR_PAYLOAD:
		kfree(expr);
		break;
	case NFT_AST_EXPR_RELATIONAL:
		nft_ast_expr_destroy(expr->relational.left);
		nft_ast_expr_destroy(expr->relational.right);
		break;
	default:
		WARN_ONCE(1, "Unknown expr %u at destroy\n", expr->type);
	}
}
EXPORT_SYMBOL_GPL(nft_ast_expr_destroy);

struct nft_ast_stmt *nft_ast_stmt_alloc(enum nft_ast_stmt_type type)
{
	struct nft_ast_stmt *stmt;

	stmt = kmalloc(sizeof(struct nft_ast_stmt), GFP_KERNEL);
	if (stmt == NULL)
		return NULL;

	stmt->type = type;
	return stmt;
}
EXPORT_SYMBOL_GPL(nft_ast_stmt_alloc);

static void nft_ast_stmt_free(struct nft_ast_stmt *stmt)
{
	nft_ast_expr_destroy(stmt->expr);
	kfree(stmt);
}

void nft_ast_stmt_list_release(struct list_head *ast_stmt_list)
{
	struct nft_ast_stmt *stmt, *next;

	list_for_each_entry_safe(stmt, next, ast_stmt_list, list) {
		list_del(&stmt->list);
		nft_ast_stmt_free(stmt);
	}
}
EXPORT_SYMBOL_GPL(nft_ast_stmt_list_release);

int nft_delinearize(struct list_head *ast_stmt_list, struct nft_rule *rule)
{
	struct nft_ast_expr *regs[NFT_REG32_15 + 1];
	struct nft_expr *expr;
	int err;

	expr = nft_expr_first(rule);
	while (expr->ops && expr != nft_expr_last(rule)) {
		if (!expr->ops->delinearize)
			return -EOPNOTSUPP;

		err = expr->ops->delinearize(regs, expr, ast_stmt_list);
		if (err < 0)
			return err;

		expr = nft_expr_next(expr);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(nft_delinearize);

/* TODO use pr_debug() here. */
static void nft_ast_expr_print(struct nft_ast_expr *expr)
{
	pr_info("expr type %u len %u\n", expr->type, expr->len);

	switch (expr->type) {
	case NFT_AST_EXPR_VALUE:
		pr_info("value %x %x %x %x\n",
			expr->value.data.data[0], expr->value.data.data[1],
			expr->value.data.data[1], expr->value.data.data[2]);
	        break;
	case NFT_AST_EXPR_META:
		pr_info("meta key %u\n", expr->meta.key);
		break;
	case NFT_AST_EXPR_PAYLOAD:
		pr_info("payload base %u offset %u\n",
			expr->payload.base, expr->payload.offset);
	break;
	case NFT_AST_EXPR_RELATIONAL:
		pr_info("relational\n");
		pr_info("       left %p\n", expr->relational.left);
		nft_ast_expr_print(expr->relational.left);
		pr_info("       right %p\n", expr->relational.right);
		nft_ast_expr_print(expr->relational.right);
		break;
	default:
		pr_info("UNKNOWN\n");
		break;
	}
}

void nft_ast_stmt_list_print(struct list_head *stmt_list)
{
	struct nft_ast_stmt *stmt;

	list_for_each_entry(stmt, stmt_list, list) {
		pr_info("stmt %u\n", stmt->type);

		switch (stmt->type) {
		case NFT_AST_STMT_EXPR:
			nft_ast_expr_print(stmt->expr);
			break;
		}
	}
}
EXPORT_SYMBOL_GPL(nft_ast_stmt_list_print);

struct nft_ast_xfrm_state {
	const struct nft_ast_xfrm_desc *xfrm_desc;
	void *data;
};

static int nft_ast_xfrm_relational(const struct nft_ast_expr *dlexpr,
				   struct nft_ast_xfrm_state *state)
{
	const struct nft_ast_expr *left = dlexpr->relational.left;
	const struct nft_ast_expr *right = dlexpr->relational.right;
	const struct nft_ast_xfrm_desc *xfrm_desc = state->xfrm_desc;
	int err;

	if (right->type != NFT_AST_EXPR_VALUE)
		return -EOPNOTSUPP;

	switch (left->type) {
	case NFT_AST_EXPR_META:
		err = xfrm_desc->meta_desc->xfrm(dlexpr, state, state->data);
		break;
	case NFT_AST_EXPR_PAYLOAD:
		err = xfrm_desc->proto_desc->xfrm(dlexpr, state, state->data);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

static int nft_ast_xfrm_expr(const struct nft_ast_expr *dlexpr,
			     struct nft_ast_xfrm_state *state)
{
	int err;

	switch (dlexpr->type) {
	case NFT_AST_EXPR_RELATIONAL:
		err = nft_ast_xfrm_relational(dlexpr, state);
		break;
	default:
		return -EOPNOTSUPP;
	}

	return err;
}

int nft_ast_xfrm(const struct list_head *ast_stmt_list,
		 const struct nft_ast_xfrm_desc *xfrm_desc, void *data)
{
	struct nft_ast_xfrm_state state = {
		.xfrm_desc	= xfrm_desc,
		.data		= data,
	};
	struct nft_ast_stmt *stmt;
	int err = 0;

	list_for_each_entry(stmt, ast_stmt_list, list) {
		switch (stmt->type) {
		case NFT_AST_STMT_EXPR:
			err = nft_ast_xfrm_expr(stmt->expr, &state);
			if (err < 0)
				return err;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}
	return err;
}
EXPORT_SYMBOL_GPL(nft_ast_xfrm);
