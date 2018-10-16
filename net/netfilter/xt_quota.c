/*
 * netfilter module to enforce network quotas
 *
 * Sam Johnston <samj@samj.net>
 */
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <linux/netfilter/x_tables.h>
#include <linux/netfilter/xt_quota.h>
#include <linux/module.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Sam Johnston <samj@samj.net>");
MODULE_DESCRIPTION("Xtables: countdown quota match");
MODULE_ALIAS("ipt_quota");
MODULE_ALIAS("ip6t_quota");

static inline bool xt_overquota(struct xt_quota_info *q,
				const struct sk_buff *skb)
{
	return atomic64_add_return(skb->len, &q->consumed) >= q->quota;
}

static bool
quota_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	struct xt_quota_info *q = (void *)par->matchinfo;

	return xt_overquota(q, skb) ^ (q->flags & XT_QUOTA_INVERT);
}

static int quota_mt_check(const struct xt_mtchk_param *par)
{
	struct xt_quota_info *q = par->matchinfo;

	BUILD_BUG_ON(sizeof(atomic64_t) != sizeof(__u64));

	if (q->flags & ~XT_QUOTA_MASK)
		return -EINVAL;
	if (atomic64_read(&q->consumed) > q->quota)
		return -ERANGE;

	return 0;
}

static struct xt_match quota_mt_reg __read_mostly = {
	.name       = "quota",
	.revision   = 0,
	.family     = NFPROTO_UNSPEC,
	.match      = quota_mt,
	.checkentry = quota_mt_check,
	.matchsize  = sizeof(struct xt_quota_info),
	.me         = THIS_MODULE,
};

static int __init quota_mt_init(void)
{
	return xt_register_match(&quota_mt_reg);
}

static void __exit quota_mt_exit(void)
{
	xt_unregister_match(&quota_mt_reg);
}

module_init(quota_mt_init);
module_exit(quota_mt_exit);
