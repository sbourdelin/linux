#ifndef _XFRM_USER_H
#define _XFRM_USER_H

#include <linux/errno.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <linux/xfrm.h>
#include <net/net_namespace.h>

struct xfrm_dump_info {
	struct sk_buff *in_skb;
	struct sk_buff *out_skb;
	u32 nlmsg_seq;
	u16 nlmsg_flags;
};

/* Common functions */

int xfrm_copy_sec_ctx(const struct xfrm_sec_ctx *s, struct sk_buff *skb);
int xfrm_copy_to_user_auth(const struct xfrm_algo_auth *auth,
			   struct sk_buff *skb);
int xfrm_verify_newpolicy_info(const struct xfrm_userpolicy_info *p);
struct xfrm_policy *xfrm_policy_construct(struct net *net,
					  const struct xfrm_userpolicy_info *p,
					  struct nlattr **attrs,
					  int *errp);
int xfrm_copy_from_user_policy_type(u8 *tp, struct nlattr **attrs);
int xfrm_verify_policy_dir(u8 dir);
int xfrm_verify_sec_ctx_len(struct nlattr **attrs);
int xfrm_nlmsg_multicast(struct net *net, struct sk_buff *skb,
			 u32 pid, unsigned int group);
int xfrm_copy_to_user_tmpl(const struct xfrm_policy *xp, struct sk_buff *skb);
size_t xfrm_sa_len(const struct xfrm_state *x);
int xfrm_verify_newsa_info(const struct xfrm_usersa_info *p,
			   struct nlattr **attrs);
struct xfrm_state *xfrm_state_construct(struct net *net,
					const struct xfrm_usersa_info *p,
					struct nlattr **attrs,
					int *errp);
struct xfrm_state *xfrm_user_state_lookup(struct net *net,
					  const struct xfrm_usersa_id *p,
					  struct nlattr **attrs,
					  int *errp);

#ifdef CONFIG_XFRM_SUB_POLICY
static inline int copy_to_user_policy_type(u8 type, struct sk_buff *skb)
{
	struct xfrm_userpolicy_type upt = {
		.type = type,
	};

	return nla_put(skb, XFRMA_POLICY_TYPE, sizeof(upt), &upt);
}

#else
static inline int copy_to_user_policy_type(u8 type, struct sk_buff *skb)
{
	return 0;
}
#endif

static inline int copy_to_user_sec_ctx(const struct xfrm_policy *xp,
				       struct sk_buff *skb)
{
	if (xp->security)
		return xfrm_copy_sec_ctx(xp->security, skb);
	return 0;
}

static inline int xfrm_user_sec_ctx_size(const struct xfrm_sec_ctx *xfrm_ctx)
{
	int len = 0;

	if (xfrm_ctx) {
		len += sizeof(struct xfrm_user_sec_ctx);
		len += xfrm_ctx->ctx_len;
	}
	return len;
}

static inline int copy_to_user_state_sec_ctx(const struct xfrm_state *x,
					     struct sk_buff *skb)
{
	if (x->security) {
		return xfrm_copy_sec_ctx(x->security, skb);
	}
	return 0;
}

/* Legacy functions */

#ifdef CONFIG_XFRM_USER_LEGACY
int xfrm_alloc_userspi_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			      struct nlattr **attrs);
int xfrm_add_pol_expire_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			       struct nlattr **attrs);
int xfrm_add_sa_expire_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			      struct nlattr **attrs);
int xfrm_add_acquire_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			    struct nlattr **attrs);

int xfrm_add_sa_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
		       struct nlattr **attrs);
int xfrm_del_sa_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
		       struct nlattr **attrs);
int xfrm_dump_sa_done_legacy(struct netlink_callback *cb);
int xfrm_dump_sa_legacy(struct sk_buff *skb, struct netlink_callback *cb);
int xfrm_get_sa_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
		       struct nlattr **attrs);
int xfrm_add_policy_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			   struct nlattr **attrs);
int xfrm_dump_policy_done_legacy(struct netlink_callback *cb);
int xfrm_dump_policy_legacy(struct sk_buff *skb, struct netlink_callback *cb);
int xfrm_get_policy_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			   struct nlattr **attrs);

int xfrm_exp_state_notify_legacy(const struct xfrm_state *x,
				 const struct km_event *c);
int xfrm_notify_sa_legacy(const struct xfrm_state *x, const struct km_event *c);
int xfrm_send_acquire_legacy(struct xfrm_state *x,
			     const struct xfrm_tmpl *xt,
			     const struct xfrm_policy *xp);
int xfrm_exp_policy_notify_legacy(const struct xfrm_policy *xp,
				  int dir,
				  const struct km_event *c);
int xfrm_notify_policy_legacy(const struct xfrm_policy *xp,
			      int dir,
			      const struct km_event *c);
#else /* CONFIG_XFRM_USER_LEGACY */
static inline int xfrm_exp_state_notify_legacy(const struct xfrm_state *x,
					       const struct km_event *c)
{
	return 0;
}

static inline int xfrm_notify_sa_legacy(const struct xfrm_state *x,
					const struct km_event *c)
{
	return 0;
}

static inline int xfrm_send_acquire_legacy(struct xfrm_state *x,
					   const struct xfrm_tmpl *xt,
					   const struct xfrm_policy *xp)
{
	return 0;
}

static inline int xfrm_exp_policy_notify_legacy(const struct xfrm_policy *xp,
						int dir,
						const struct km_event *c)
{
	return 0;
}

static inline int xfrm_notify_policy_legacy(const struct xfrm_policy *xp,
					    int dir,
					    const struct km_event *c)
{
	return 0;
}
#endif /* CONFIG_XFRM_USER_LEGACY */

#endif /* _XFRM_USER_H */
