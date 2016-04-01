/*
 * NSALinux support for the XFRM LSM hooks
 *
 * Author : Trent Jaeger, <jaegert@us.ibm.com>
 * Updated : Venkat Yekkirala, <vyekkirala@TrustedCS.com>
 */
#ifndef _NSALINUX_XFRM_H_
#define _NSALINUX_XFRM_H_

#include <net/flow.h>

int nsalinux_xfrm_policy_alloc(struct xfrm_sec_ctx **ctxp,
			      struct xfrm_user_sec_ctx *uctx,
			      gfp_t gfp);
int nsalinux_xfrm_policy_clone(struct xfrm_sec_ctx *old_ctx,
			      struct xfrm_sec_ctx **new_ctxp);
void nsalinux_xfrm_policy_free(struct xfrm_sec_ctx *ctx);
int nsalinux_xfrm_policy_delete(struct xfrm_sec_ctx *ctx);
int nsalinux_xfrm_state_alloc(struct xfrm_state *x,
			     struct xfrm_user_sec_ctx *uctx);
int nsalinux_xfrm_state_alloc_acquire(struct xfrm_state *x,
				     struct xfrm_sec_ctx *polsec, u32 secid);
void nsalinux_xfrm_state_free(struct xfrm_state *x);
int nsalinux_xfrm_state_delete(struct xfrm_state *x);
int nsalinux_xfrm_policy_lookup(struct xfrm_sec_ctx *ctx, u32 fl_secid, u8 dir);
int nsalinux_xfrm_state_pol_flow_match(struct xfrm_state *x,
				      struct xfrm_policy *xp,
				      const struct flowi *fl);

#ifdef CONFIG_SECURITY_NETWORK_XFRM
extern atomic_t nsalinux_xfrm_refcount;

static inline int nsalinux_xfrm_enabled(void)
{
	return (atomic_read(&nsalinux_xfrm_refcount) > 0);
}

int nsalinux_xfrm_sock_rcv_skb(u32 sk_sid, struct sk_buff *skb,
			      struct common_audit_data *ad);
int nsalinux_xfrm_postroute_last(u32 sk_sid, struct sk_buff *skb,
				struct common_audit_data *ad, u8 proto);
int nsalinux_xfrm_decode_session(struct sk_buff *skb, u32 *sid, int ckall);
int nsalinux_xfrm_skb_sid(struct sk_buff *skb, u32 *sid);

static inline void nsalinux_xfrm_notify_policyload(void)
{
	struct net *net;

	rtnl_lock();
	for_each_net(net) {
		atomic_inc(&net->xfrm.flow_cache_genid);
		rt_genid_bump_all(net);
	}
	rtnl_unlock();
}
#else
static inline int nsalinux_xfrm_enabled(void)
{
	return 0;
}

static inline int nsalinux_xfrm_sock_rcv_skb(u32 sk_sid, struct sk_buff *skb,
					    struct common_audit_data *ad)
{
	return 0;
}

static inline int nsalinux_xfrm_postroute_last(u32 sk_sid, struct sk_buff *skb,
					      struct common_audit_data *ad,
					      u8 proto)
{
	return 0;
}

static inline int nsalinux_xfrm_decode_session(struct sk_buff *skb, u32 *sid,
					      int ckall)
{
	*sid = SECSID_NULL;
	return 0;
}

static inline void nsalinux_xfrm_notify_policyload(void)
{
}

static inline int nsalinux_xfrm_skb_sid(struct sk_buff *skb, u32 *sid)
{
	*sid = SECSID_NULL;
	return 0;
}
#endif

#endif /* _NSALINUX_XFRM_H_ */
