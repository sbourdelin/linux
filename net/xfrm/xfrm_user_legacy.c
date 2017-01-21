/* xfrm_user.c: User interface to configure xfrm engine.
 *
 * Copyright (C) 2002 David S. Miller (davem@redhat.com)
 *
 * Changes:
 *	Mitsuru KANDA @USAGI
 * 	Kazunori MIYAZAWA @USAGI
 * 	Kunihiro Ishiguro <kunihiro@ipinfusion.com>
 * 		IPv6 support
 *
 */

#include <linux/crypto.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/net.h>
#include <linux/skbuff.h>
#include <linux/pfkeyv2.h>
#include <linux/ipsec.h>
#include <linux/security.h>
#include <net/sock.h>
#include <net/xfrm.h>
#include <net/netlink.h>
#include <net/ah.h>
#include <linux/uaccess.h>
#include <asm/unaligned.h>
#include "xfrm_user.h"

int xfrm_add_sa_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
		       struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	const struct xfrm_usersa_info_legacy *p = nlmsg_data(nlh);
	struct xfrm_state *x;
	int err;
	struct km_event c;

	/* This cast is safe because the only difference is end padding. */
	err = xfrm_verify_newsa_info((const struct xfrm_usersa_info *)p, attrs);
	if (err)
		return err;

	x = xfrm_state_construct(net, (const struct xfrm_usersa_info *)p,
				 attrs, &err);
	if (!x)
		return err;

	xfrm_state_hold(x);
	if (nlh->nlmsg_type == XFRM_MSG_NEWSA_LEGACY) {
		err = xfrm_state_add(x);
		c.event = XFRM_MSG_NEWSA;
	} else {
		err = xfrm_state_update(x);
		c.event = XFRM_MSG_UPDSA;
	}

	xfrm_audit_state_add(x, err ? 0 : 1, true);

	if (err < 0) {
		x->km.state = XFRM_STATE_DEAD;
		__xfrm_state_put(x);
		goto out;
	}

	c.seq = nlh->nlmsg_seq;
	c.portid = nlh->nlmsg_pid;

	km_state_notify(x, &c);
out:
	xfrm_state_put(x);
	return err;
}

int xfrm_del_sa_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
		       struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	struct xfrm_state *x;
	int err = -ESRCH;
	struct km_event c;
	const struct xfrm_usersa_id *p = nlmsg_data(nlh);

	x = xfrm_user_state_lookup(net, p, attrs, &err);
	if (x == NULL)
		return err;

	if ((err = security_xfrm_state_delete(x)) != 0)
		goto out;

	if (xfrm_state_kern(x)) {
		err = -EPERM;
		goto out;
	}

	err = xfrm_state_delete(x);

	if (err < 0)
		goto out;

	c.seq = nlh->nlmsg_seq;
	c.portid = nlh->nlmsg_pid;
	c.event = XFRM_MSG_DELSA;
	km_state_notify(x, &c);

out:
	xfrm_audit_state_delete(x, err ? 0 : 1, true);
	xfrm_state_put(x);
	return err;
}

static void copy_to_user_state(const struct xfrm_state *x,
			       struct xfrm_usersa_info_legacy *p)
{
	memset(p, 0, sizeof(*p));
	memcpy(&p->id, &x->id, sizeof(p->id));
	memcpy(&p->sel, &x->sel, sizeof(p->sel));
	memcpy(&p->lft, &x->lft, sizeof(p->lft));
	memcpy(&p->curlft, &x->curlft, sizeof(p->curlft));
	put_unaligned(x->stats.replay_window, &p->stats.replay_window);
	put_unaligned(x->stats.replay, &p->stats.replay);
	put_unaligned(x->stats.integrity_failed, &p->stats.integrity_failed);
	memcpy(&p->saddr, &x->props.saddr, sizeof(p->saddr));
	p->mode = x->props.mode;
	p->replay_window = x->props.replay_window;
	p->reqid = x->props.reqid;
	p->family = x->props.family;
	p->flags = x->props.flags;
	p->seq = x->km.seq;
}

static int copy_to_user_state_extra(const struct xfrm_state *x,
				    struct xfrm_usersa_info_legacy *p,
				    struct sk_buff *skb)
{
	int ret = 0;

	copy_to_user_state(x, p);

	if (x->props.extra_flags) {
		ret = nla_put_u32(skb, XFRMA_SA_EXTRA_FLAGS,
				  x->props.extra_flags);
		if (ret)
			goto out;
	}

	if (x->coaddr) {
		ret = nla_put(skb, XFRMA_COADDR, sizeof(*x->coaddr), x->coaddr);
		if (ret)
			goto out;
	}
	if (x->lastused) {
		ret = nla_put_u64_64bit(skb, XFRMA_LASTUSED, x->lastused,
					XFRMA_PAD);
		if (ret)
			goto out;
	}
	if (x->aead) {
		ret = nla_put(skb, XFRMA_ALG_AEAD, aead_len(x->aead), x->aead);
		if (ret)
			goto out;
	}
	if (x->aalg) {
		ret = xfrm_copy_to_user_auth(x->aalg, skb);
		if (!ret)
			ret = nla_put(skb, XFRMA_ALG_AUTH_TRUNC,
				      xfrm_alg_auth_len(x->aalg), x->aalg);
		if (ret)
			goto out;
	}
	if (x->ealg) {
		ret = nla_put(skb, XFRMA_ALG_CRYPT, xfrm_alg_len(x->ealg), x->ealg);
		if (ret)
			goto out;
	}
	if (x->calg) {
		ret = nla_put(skb, XFRMA_ALG_COMP, sizeof(*(x->calg)), x->calg);
		if (ret)
			goto out;
	}
	if (x->encap) {
		ret = nla_put(skb, XFRMA_ENCAP, sizeof(*x->encap), x->encap);
		if (ret)
			goto out;
	}
	if (x->tfcpad) {
		ret = nla_put_u32(skb, XFRMA_TFCPAD, x->tfcpad);
		if (ret)
			goto out;
	}
	ret = xfrm_mark_put(skb, &x->mark);
	if (ret)
		goto out;
	if (x->replay_esn)
		ret = nla_put(skb, XFRMA_REPLAY_ESN_VAL,
			      xfrm_replay_state_esn_len(x->replay_esn),
			      x->replay_esn);
	else
		ret = nla_put(skb, XFRMA_REPLAY_VAL, sizeof(x->replay),
			      &x->replay);
	if (ret)
		goto out;
	if (x->security)
		ret = xfrm_copy_sec_ctx(x->security, skb);
out:
	return ret;
}

static int dump_one_state(const struct xfrm_state *x, int count, void *ptr)
{
	struct xfrm_dump_info *sp = ptr;
	struct sk_buff *in_skb = sp->in_skb;
	struct sk_buff *skb = sp->out_skb;
	struct xfrm_usersa_info_legacy *p;
	struct nlmsghdr *nlh;
	int err;

	nlh = nlmsg_put(skb, NETLINK_CB(in_skb).portid, sp->nlmsg_seq,
			XFRM_MSG_NEWSA_LEGACY, sizeof(*p), sp->nlmsg_flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	p = nlmsg_data(nlh);

	err = copy_to_user_state_extra(x, p, skb);
	if (err) {
		nlmsg_cancel(skb, nlh);
		return err;
	}
	nlmsg_end(skb, nlh);
	return 0;
}

int xfrm_dump_sa_done_legacy(struct netlink_callback *cb)
{
	struct xfrm_state_walk *walk = (struct xfrm_state_walk *) &cb->args[1];
	struct sock *sk = cb->skb->sk;
	struct net *net = sock_net(sk);

	if (cb->args[0])
		xfrm_state_walk_done(walk, net);
	return 0;
}

static const struct nla_policy xfrma_policy[XFRMA_MAX+1];
int xfrm_dump_sa_legacy(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct xfrm_state_walk *walk = (struct xfrm_state_walk *) &cb->args[1];
	struct xfrm_dump_info info;

	BUILD_BUG_ON(sizeof(struct xfrm_state_walk) >
		     sizeof(cb->args) - sizeof(cb->args[0]));

	info.in_skb = cb->skb;
	info.out_skb = skb;
	info.nlmsg_seq = cb->nlh->nlmsg_seq;
	info.nlmsg_flags = NLM_F_MULTI;

	if (!cb->args[0]) {
		struct nlattr *attrs[XFRMA_MAX+1];
		struct xfrm_address_filter *filter = NULL;
		u8 proto = 0;
		int err;

		err = nlmsg_parse(cb->nlh, 0, attrs, XFRMA_MAX,
				  xfrma_policy);
		if (err < 0)
			return err;

		if (attrs[XFRMA_ADDRESS_FILTER]) {
			filter = kmemdup(nla_data(attrs[XFRMA_ADDRESS_FILTER]),
					 sizeof(*filter), GFP_KERNEL);
			if (filter == NULL)
				return -ENOMEM;
		}

		if (attrs[XFRMA_PROTO])
			proto = nla_get_u8(attrs[XFRMA_PROTO]);

		xfrm_state_walk_init(walk, proto, filter);
		cb->args[0] = 1;
	}

	(void) xfrm_state_walk(net, walk, dump_one_state, &info);

	return skb->len;
}

static struct sk_buff *xfrm_state_netlink(struct sk_buff *in_skb,
					  const struct xfrm_state *x,
					  u32 seq)
{
	struct xfrm_dump_info info;
	struct sk_buff *skb;
	int err;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_ATOMIC);
	if (!skb)
		return ERR_PTR(-ENOMEM);

	info.in_skb = in_skb;
	info.out_skb = skb;
	info.nlmsg_seq = seq;
	info.nlmsg_flags = 0;

	err = dump_one_state(x, 0, &info);
	if (err) {
		kfree_skb(skb);
		return ERR_PTR(err);
	}

	return skb;
}

int xfrm_get_sa_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
		       struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	const struct xfrm_usersa_id *p = nlmsg_data(nlh);
	struct xfrm_state *x;
	struct sk_buff *resp_skb;
	int err = -ESRCH;

	x = xfrm_user_state_lookup(net, p, attrs, &err);
	if (x == NULL)
		goto out_noput;

	resp_skb = xfrm_state_netlink(skb, x, nlh->nlmsg_seq);
	if (IS_ERR(resp_skb)) {
		err = PTR_ERR(resp_skb);
	} else {
		err = nlmsg_unicast(net->xfrm.nlsk, resp_skb, NETLINK_CB(skb).portid);
	}
	xfrm_state_put(x);
out_noput:
	return err;
}

int xfrm_alloc_userspi_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			      struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	struct xfrm_state *x;
	const struct xfrm_userspi_info_legacy *p;
	struct sk_buff *resp_skb;
	const xfrm_address_t *daddr;
	int family;
	int err;
	u32 mark;
	struct xfrm_mark m;

	p = nlmsg_data(nlh);
	err = verify_spi_info(p->info.id.proto, p->min, p->max);
	if (err)
		goto out_noput;

	family = p->info.family;
	daddr = &p->info.id.daddr;

	x = NULL;

	mark = xfrm_mark_get(attrs, &m);
	if (p->info.seq) {
		x = xfrm_find_acq_byseq(net, mark, p->info.seq);
		if (x && !xfrm_addr_equal(&x->id.daddr, daddr, family)) {
			xfrm_state_put(x);
			x = NULL;
		}
	}

	if (!x)
		x = xfrm_find_acq(net, &m, p->info.mode, p->info.reqid,
				  p->info.id.proto, daddr,
				  &p->info.saddr, 1,
				  family);
	err = -ENOENT;
	if (x == NULL)
		goto out_noput;

	err = xfrm_alloc_spi(x, p->min, p->max);
	if (err)
		goto out;

	resp_skb = xfrm_state_netlink(skb, x, nlh->nlmsg_seq);
	if (IS_ERR(resp_skb)) {
		err = PTR_ERR(resp_skb);
		goto out;
	}

	err = nlmsg_unicast(net->xfrm.nlsk, resp_skb, NETLINK_CB(skb).portid);

out:
	xfrm_state_put(x);
out_noput:
	return err;
}

static void copy_to_user_policy(const struct xfrm_policy *xp,
				struct xfrm_userpolicy_info_legacy *p,
				int dir)
{
	memset(p, 0, sizeof(*p));
	memcpy(&p->sel, &xp->selector, sizeof(p->sel));
	memcpy(&p->lft, &xp->lft, sizeof(p->lft));
	memcpy(&p->curlft, &xp->curlft, sizeof(p->curlft));
	p->priority = xp->priority;
	p->index = xp->index;
	p->sel.family = xp->family;
	p->dir = dir;
	p->action = xp->action;
	p->flags = xp->flags;
	p->share = XFRM_SHARE_ANY; /* XXX xp->share */
}

int xfrm_add_policy_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			   struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	const struct xfrm_userpolicy_info_legacy *p = nlmsg_data(nlh);
	struct xfrm_policy *xp;
	struct km_event c;
	int err;
	int excl;

	/* This cast is safe because the only difference is end padding. */
	err = xfrm_verify_newpolicy_info(
		(const struct xfrm_userpolicy_info *)p);
	if (err)
		return err;
	err = xfrm_verify_sec_ctx_len(attrs);
	if (err)
		return err;

	xp = xfrm_policy_construct(net, (const struct xfrm_userpolicy_info *)p,
				   attrs, &err);
	if (!xp)
		return err;

	/* shouldn't excl be based on nlh flags??
	 * Aha! this is anti-netlink really i.e  more pfkey derived
	 * in netlink excl is a flag and you wouldnt need
	 * a type XFRM_MSG_UPDPOLICY - JHS */
	if (nlh->nlmsg_type == XFRM_MSG_NEWPOLICY_LEGACY) {
		excl = 1;
		c.event = XFRM_MSG_NEWPOLICY;
	} else {
		excl = 0;
		c.event = XFRM_MSG_UPDPOLICY;
	}
	err = xfrm_policy_insert(p->dir, xp, excl);
	xfrm_audit_policy_add(xp, err ? 0 : 1, true);

	if (err) {
		security_xfrm_policy_free(xp->security);
		kfree(xp);
		return err;
	}

	c.seq = nlh->nlmsg_seq;
	c.portid = nlh->nlmsg_pid;
	km_policy_notify(xp, p->dir, &c);

	xfrm_pol_put(xp);

	return 0;
}

static inline size_t userpolicy_type_attrsize(void)
{
#ifdef CONFIG_XFRM_SUB_POLICY
	return nla_total_size(sizeof(struct xfrm_userpolicy_type));
#else
	return 0;
#endif
}

static int dump_one_policy(const struct xfrm_policy *xp,
			   int dir,
			   int count,
			   void *ptr)
{
	struct xfrm_dump_info *sp = ptr;
	struct xfrm_userpolicy_info_legacy *p;
	struct sk_buff *in_skb = sp->in_skb;
	struct sk_buff *skb = sp->out_skb;
	struct nlmsghdr *nlh;
	int err;

	nlh = nlmsg_put(skb, NETLINK_CB(in_skb).portid, sp->nlmsg_seq,
			XFRM_MSG_NEWPOLICY_LEGACY, sizeof(*p), sp->nlmsg_flags);
	if (nlh == NULL)
		return -EMSGSIZE;

	p = nlmsg_data(nlh);
	copy_to_user_policy(xp, p, dir);
	err = xfrm_copy_to_user_tmpl(xp, skb);
	if (!err)
		err = copy_to_user_sec_ctx(xp, skb);
	if (!err)
		err = copy_to_user_policy_type(xp->type, skb);
	if (!err)
		err = xfrm_mark_put(skb, &xp->mark);
	if (err) {
		nlmsg_cancel(skb, nlh);
		return err;
	}
	nlmsg_end(skb, nlh);
	return 0;
}

int xfrm_dump_policy_done_legacy(struct netlink_callback *cb)
{
	struct xfrm_policy_walk *walk = (struct xfrm_policy_walk *) &cb->args[1];
	struct net *net = sock_net(cb->skb->sk);

	xfrm_policy_walk_done(walk, net);
	return 0;
}

int xfrm_dump_policy_legacy(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct net *net = sock_net(skb->sk);
	struct xfrm_policy_walk *walk = (struct xfrm_policy_walk *) &cb->args[1];
	struct xfrm_dump_info info;

	BUILD_BUG_ON(sizeof(struct xfrm_policy_walk) >
		     sizeof(cb->args) - sizeof(cb->args[0]));

	info.in_skb = cb->skb;
	info.out_skb = skb;
	info.nlmsg_seq = cb->nlh->nlmsg_seq;
	info.nlmsg_flags = NLM_F_MULTI;

	if (!cb->args[0]) {
		cb->args[0] = 1;
		xfrm_policy_walk_init(walk, XFRM_POLICY_TYPE_ANY);
	}

	(void) xfrm_policy_walk(net, walk, dump_one_policy, &info);

	return skb->len;
}

static struct sk_buff *xfrm_policy_netlink(struct sk_buff *in_skb,
					   const struct xfrm_policy *xp,
					   int dir,
					   u32 seq)
{
	struct xfrm_dump_info info;
	struct sk_buff *skb;
	int err;

	skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (!skb)
		return ERR_PTR(-ENOMEM);

	info.in_skb = in_skb;
	info.out_skb = skb;
	info.nlmsg_seq = seq;
	info.nlmsg_flags = 0;

	err = dump_one_policy(xp, dir, 0, &info);
	if (err) {
		kfree_skb(skb);
		return ERR_PTR(err);
	}

	return skb;
}

int xfrm_get_policy_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			   struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	struct xfrm_policy *xp;
	const struct xfrm_userpolicy_id *p;
	u8 type = XFRM_POLICY_TYPE_MAIN;
	int err;
	struct km_event c;
	int delete;
	struct xfrm_mark m;
	u32 mark = xfrm_mark_get(attrs, &m);

	p = nlmsg_data(nlh);
	if (nlh->nlmsg_type == XFRM_MSG_DELPOLICY_LEGACY) {
		delete = 1;
		c.event = XFRM_MSG_DELPOLICY;
	} else {
		delete = 0;
		c.event = XFRM_MSG_GETPOLICY;
	}

	err = xfrm_copy_from_user_policy_type(&type, attrs);
	if (err)
		return err;

	err = xfrm_verify_policy_dir(p->dir);
	if (err)
		return err;

	if (p->index)
		xp = xfrm_policy_byid(net, mark, type, p->dir, p->index, delete, &err);
	else {
		struct nlattr *rt = attrs[XFRMA_SEC_CTX];
		struct xfrm_sec_ctx *ctx;

		err = xfrm_verify_sec_ctx_len(attrs);
		if (err)
			return err;

		ctx = NULL;
		if (rt) {
			struct xfrm_user_sec_ctx *uctx = nla_data(rt);

			err = security_xfrm_policy_alloc(&ctx, uctx, GFP_KERNEL);
			if (err)
				return err;
		}
		xp = xfrm_policy_bysel_ctx(net, mark, type, p->dir, &p->sel,
					   ctx, delete, &err);
		security_xfrm_policy_free(ctx);
	}
	if (xp == NULL)
		return -ENOENT;

	if (!delete) {
		struct sk_buff *resp_skb;

		resp_skb = xfrm_policy_netlink(skb, xp, p->dir, nlh->nlmsg_seq);
		if (IS_ERR(resp_skb)) {
			err = PTR_ERR(resp_skb);
		} else {
			err = nlmsg_unicast(net->xfrm.nlsk, resp_skb,
					    NETLINK_CB(skb).portid);
		}
	} else {
		xfrm_audit_policy_delete(xp, err ? 0 : 1, true);

		if (err != 0)
			goto out;

		c.data.byid = p->index;
		c.seq = nlh->nlmsg_seq;
		c.portid = nlh->nlmsg_pid;
		km_policy_notify(xp, p->dir, &c);
	}

out:
	xfrm_pol_put(xp);
	if (delete && err == 0)
		xfrm_garbage_collect(net);
	return err;
}

int xfrm_add_pol_expire_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			       struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	struct xfrm_policy *xp;
	const struct xfrm_user_polexpire_legacy *up = nlmsg_data(nlh);
	const struct xfrm_userpolicy_info_legacy *p = &up->pol;
	u8 type = XFRM_POLICY_TYPE_MAIN;
	int err = -ENOENT;
	struct xfrm_mark m;
	u32 mark = xfrm_mark_get(attrs, &m);

	err = xfrm_copy_from_user_policy_type(&type, attrs);
	if (err)
		return err;

	err = xfrm_verify_policy_dir(p->dir);
	if (err)
		return err;

	if (p->index)
		xp = xfrm_policy_byid(net, mark, type, p->dir, p->index, 0, &err);
	else {
		struct nlattr *rt = attrs[XFRMA_SEC_CTX];
		struct xfrm_sec_ctx *ctx;

		err = xfrm_verify_sec_ctx_len(attrs);
		if (err)
			return err;

		ctx = NULL;
		if (rt) {
			struct xfrm_user_sec_ctx *uctx = nla_data(rt);

			err = security_xfrm_policy_alloc(&ctx, uctx, GFP_KERNEL);
			if (err)
				return err;
		}
		xp = xfrm_policy_bysel_ctx(net, mark, type, p->dir,
					   &p->sel, ctx, 0, &err);
		security_xfrm_policy_free(ctx);
	}
	if (xp == NULL)
		return -ENOENT;

	if (unlikely(xp->walk.dead))
		goto out;

	err = 0;
	if (up->hard) {
		xfrm_policy_delete(xp, p->dir);
		xfrm_audit_policy_delete(xp, 1, true);
	}
	km_policy_expired(xp, p->dir, up->hard, nlh->nlmsg_pid);

out:
	xfrm_pol_put(xp);
	return err;
}

int xfrm_add_sa_expire_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			      struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	struct xfrm_state *x;
	int err;
	const struct xfrm_user_expire_legacy *ue = nlmsg_data(nlh);
	const struct xfrm_usersa_info_legacy *p = &ue->state;
	struct xfrm_mark m;
	u32 mark = xfrm_mark_get(attrs, &m);

	x = xfrm_state_lookup(net, mark, &p->id.daddr, p->id.spi, p->id.proto, p->family);

	err = -ENOENT;
	if (x == NULL)
		return err;

	spin_lock_bh(&x->lock);
	err = -EINVAL;
	if (x->km.state != XFRM_STATE_VALID)
		goto out;
	km_state_expired(x, ue->hard, nlh->nlmsg_pid);

	if (ue->hard) {
		__xfrm_state_delete(x);
		xfrm_audit_state_delete(x, 1, true);
	}
	err = 0;
out:
	spin_unlock_bh(&x->lock);
	xfrm_state_put(x);
	return err;
}

int xfrm_add_acquire_legacy(struct sk_buff *skb, const struct nlmsghdr *nlh,
			    struct nlattr **attrs)
{
	struct net *net = sock_net(skb->sk);
	struct xfrm_policy *xp;
	struct xfrm_user_tmpl *ut;
	int i;
	struct nlattr *rt = attrs[XFRMA_TMPL];
	struct xfrm_mark mark;

	const struct xfrm_user_acquire_legacy *ua = nlmsg_data(nlh);
	struct xfrm_state *x = xfrm_state_alloc(net);
	int err = -ENOMEM;

	if (!x)
		goto nomem;

	xfrm_mark_get(attrs, &mark);

	/* This cast is safe because the only difference is end padding. */
	err = xfrm_verify_newpolicy_info(
		(const struct xfrm_userpolicy_info *)&ua->policy);
	if (err)
		goto free_state;

	/*   build an XP */
	xp = xfrm_policy_construct(net, (const struct xfrm_userpolicy_info *)
				   &ua->policy, attrs, &err);
	if (!xp)
		goto free_state;

	memcpy(&x->id, &ua->id, sizeof(ua->id));
	memcpy(&x->props.saddr, &ua->saddr, sizeof(ua->saddr));
	memcpy(&x->sel, &ua->sel, sizeof(ua->sel));
	xp->mark.m = x->mark.m = mark.m;
	xp->mark.v = x->mark.v = mark.v;
	ut = nla_data(rt);
	/* extract the templates and for each call km_key */
	for (i = 0; i < xp->xfrm_nr; i++, ut++) {
		struct xfrm_tmpl *t = &xp->xfrm_vec[i];
		memcpy(&x->id, &t->id, sizeof(x->id));
		x->props.mode = t->mode;
		x->props.reqid = t->reqid;
		x->props.family = ut->family;
		t->aalgos = ua->aalgos;
		t->ealgos = ua->ealgos;
		t->calgos = ua->calgos;
		err = km_query(x, t, xp);

	}

	kfree(x);
	kfree(xp);

	return 0;

free_state:
	kfree(x);
nomem:
	return err;
}

static inline size_t xfrm_expire_msgsize(void)
{
	return NLMSG_ALIGN(sizeof(struct xfrm_user_expire_legacy))
	       + nla_total_size(sizeof(struct xfrm_mark));
}

static int build_expire(struct sk_buff *skb,
			const struct xfrm_state *x,
			const struct km_event *c)
{
	struct xfrm_user_expire_legacy *ue;
	struct nlmsghdr *nlh;
	int err;

	nlh = nlmsg_put(skb, c->portid, 0, XFRM_MSG_EXPIRE_LEGACY,
			sizeof(*ue), 0);
	if (nlh == NULL)
		return -EMSGSIZE;

	ue = nlmsg_data(nlh);
	copy_to_user_state(x, &ue->state);
	ue->hard = (c->data.hard != 0) ? 1 : 0;

	err = xfrm_mark_put(skb, &x->mark);
	if (err)
		return err;

	nlmsg_end(skb, nlh);
	return 0;
}

int xfrm_exp_state_notify_legacy(const struct xfrm_state *x,
				 const struct km_event *c)
{
	struct net *net = xs_net(x);
	struct sk_buff *skb;

	skb = nlmsg_new(xfrm_expire_msgsize(), GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	if (build_expire(skb, x, c) < 0) {
		kfree_skb(skb);
		return -EMSGSIZE;
	}

	return xfrm_nlmsg_multicast(net, skb, 0, XFRMNLGRP_EXPIRE);
}

int xfrm_notify_sa_legacy(const struct xfrm_state *x, const struct km_event *c)
{
	struct net *net = xs_net(x);
	struct xfrm_usersa_info_legacy *p;
	struct xfrm_usersa_id *id;
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	int len = xfrm_sa_len(x);
	int headlen, err;
	u32 event = 0;

	headlen = sizeof(*p);
	if (c->event == XFRM_MSG_DELSA) {
		len += nla_total_size(headlen);
		headlen = sizeof(*id);
		len += nla_total_size(sizeof(struct xfrm_mark));
	}
	len += NLMSG_ALIGN(headlen);

	skb = nlmsg_new(len, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	switch (c->event) {
	case XFRM_MSG_NEWSA:
		event = XFRM_MSG_NEWSA_LEGACY;
		break;
	case XFRM_MSG_UPDSA:
		event = XFRM_MSG_UPDSA_LEGACY;
		break;
	case XFRM_MSG_DELSA:
		event = XFRM_MSG_DELSA_LEGACY;
		break;
	}

	nlh = nlmsg_put(skb, c->portid, c->seq, event, headlen, 0);
	err = -EMSGSIZE;
	if (nlh == NULL)
		goto out_free_skb;

	p = nlmsg_data(nlh);
	if (c->event == XFRM_MSG_DELSA) {
		struct nlattr *attr;

		id = nlmsg_data(nlh);
		memcpy(&id->daddr, &x->id.daddr, sizeof(id->daddr));
		id->spi = x->id.spi;
		id->family = x->props.family;
		id->proto = x->id.proto;

		attr = nla_reserve(skb, XFRMA_SA, sizeof(*p));
		err = -EMSGSIZE;
		if (attr == NULL)
			goto out_free_skb;

		p = nla_data(attr);
	}
	err = copy_to_user_state_extra(x, p, skb);
	if (err)
		goto out_free_skb;

	nlmsg_end(skb, nlh);

	return xfrm_nlmsg_multicast(net, skb, 0, XFRMNLGRP_SA);

out_free_skb:
	kfree_skb(skb);
	return err;
}

static inline size_t xfrm_acquire_msgsize(const struct xfrm_state *x,
					  const struct xfrm_policy *xp)
{
	return NLMSG_ALIGN(sizeof(struct xfrm_user_acquire_legacy))
	       + nla_total_size(sizeof(struct xfrm_user_tmpl) * xp->xfrm_nr)
	       + nla_total_size(sizeof(struct xfrm_mark))
	       + nla_total_size(xfrm_user_sec_ctx_size(x->security))
	       + userpolicy_type_attrsize();
}

static int build_acquire(struct sk_buff *skb,
			 struct xfrm_state *x,
			 const struct xfrm_tmpl *xt,
			 const struct xfrm_policy *xp)
{
	__u32 seq = xfrm_get_acqseq();
	struct xfrm_user_acquire_legacy *ua;
	struct nlmsghdr *nlh;
	int err;

	nlh = nlmsg_put(skb, 0, 0, XFRM_MSG_ACQUIRE_LEGACY, sizeof(*ua), 0);
	if (nlh == NULL)
		return -EMSGSIZE;

	ua = nlmsg_data(nlh);
	memcpy(&ua->id, &x->id, sizeof(ua->id));
	memcpy(&ua->saddr, &x->props.saddr, sizeof(ua->saddr));
	memcpy(&ua->sel, &x->sel, sizeof(ua->sel));
	copy_to_user_policy(xp, &ua->policy, XFRM_POLICY_OUT);
	ua->aalgos = xt->aalgos;
	ua->ealgos = xt->ealgos;
	ua->calgos = xt->calgos;
	ua->seq = x->km.seq = seq;

	err = xfrm_copy_to_user_tmpl(xp, skb);
	if (!err)
		err = copy_to_user_state_sec_ctx(x, skb);
	if (!err)
		err = copy_to_user_policy_type(xp->type, skb);
	if (!err)
		err = xfrm_mark_put(skb, &xp->mark);
	if (err) {
		nlmsg_cancel(skb, nlh);
		return err;
	}

	nlmsg_end(skb, nlh);
	return 0;
}

int xfrm_send_acquire_legacy(struct xfrm_state *x,
			     const struct xfrm_tmpl *xt,
			     const struct xfrm_policy *xp)
{
	struct net *net = xs_net(x);
	struct sk_buff *skb;

	skb = nlmsg_new(xfrm_acquire_msgsize(x, xp), GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	if (build_acquire(skb, x, xt, xp) < 0)
		BUG();

	return xfrm_nlmsg_multicast(net, skb, 0, XFRMNLGRP_ACQUIRE);
}

static inline size_t xfrm_polexpire_msgsize(const struct xfrm_policy *xp)
{
	return NLMSG_ALIGN(sizeof(struct xfrm_user_polexpire_legacy))
	       + nla_total_size(sizeof(struct xfrm_user_tmpl) * xp->xfrm_nr)
	       + nla_total_size(xfrm_user_sec_ctx_size(xp->security))
	       + nla_total_size(sizeof(struct xfrm_mark))
	       + userpolicy_type_attrsize();
}

static int build_polexpire(struct sk_buff *skb,
			   const struct xfrm_policy *xp,
			   int dir,
			   const struct km_event *c)
{
	struct xfrm_user_polexpire_legacy *upe;
	int hard = c->data.hard;
	struct nlmsghdr *nlh;
	int err;

	nlh = nlmsg_put(skb, c->portid, 0, XFRM_MSG_POLEXPIRE_LEGACY,
			sizeof(*upe), 0);
	if (nlh == NULL)
		return -EMSGSIZE;

	upe = nlmsg_data(nlh);
	copy_to_user_policy(xp, &upe->pol, dir);
	err = xfrm_copy_to_user_tmpl(xp, skb);
	if (!err)
		err = copy_to_user_sec_ctx(xp, skb);
	if (!err)
		err = copy_to_user_policy_type(xp->type, skb);
	if (!err)
		err = xfrm_mark_put(skb, &xp->mark);
	if (err) {
		nlmsg_cancel(skb, nlh);
		return err;
	}
	upe->hard = !!hard;

	nlmsg_end(skb, nlh);
	return 0;
}

int xfrm_exp_policy_notify_legacy(const struct xfrm_policy *xp,
				  int dir,
				  const struct km_event *c)
{
	struct net *net = xp_net(xp);
	struct sk_buff *skb;

	skb = nlmsg_new(xfrm_polexpire_msgsize(xp), GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	if (build_polexpire(skb, xp, dir, c) < 0)
		BUG();

	return xfrm_nlmsg_multicast(net, skb, 0, XFRMNLGRP_EXPIRE);
}

int xfrm_notify_policy_legacy(const struct xfrm_policy *xp,
			      int dir,
			      const struct km_event *c)
{
	int len = nla_total_size(sizeof(struct xfrm_user_tmpl) * xp->xfrm_nr);
	struct net *net = xp_net(xp);
	struct xfrm_userpolicy_info_legacy *p;
	struct xfrm_userpolicy_id *id;
	struct nlmsghdr *nlh;
	struct sk_buff *skb;
	int headlen, err;
	u32 event = 0;

	headlen = sizeof(*p);

	if (c->event == XFRM_MSG_DELPOLICY) {
		len += nla_total_size(headlen);
		headlen = sizeof(*id);
	}
	len += userpolicy_type_attrsize();
	len += nla_total_size(sizeof(struct xfrm_mark));
	len += NLMSG_ALIGN(headlen);

	skb = nlmsg_new(len, GFP_ATOMIC);
	if (skb == NULL)
		return -ENOMEM;

	switch (c->event) {
	case XFRM_MSG_NEWPOLICY:
		event = XFRM_MSG_NEWPOLICY_LEGACY;
		break;
	case XFRM_MSG_UPDPOLICY:
		event = XFRM_MSG_UPDPOLICY_LEGACY;
		break;
	case XFRM_MSG_DELPOLICY:
		event = XFRM_MSG_DELPOLICY_LEGACY;
		break;
	}

	nlh = nlmsg_put(skb, c->portid, c->seq, event, headlen, 0);
	err = -EMSGSIZE;
	if (nlh == NULL)
		goto out_free_skb;

	p = nlmsg_data(nlh);
	if (c->event == XFRM_MSG_DELPOLICY) {
		struct nlattr *attr;

		id = nlmsg_data(nlh);
		memset(id, 0, sizeof(*id));
		id->dir = dir;
		if (c->data.byid)
			id->index = xp->index;
		else
			memcpy(&id->sel, &xp->selector, sizeof(id->sel));

		attr = nla_reserve(skb, XFRMA_POLICY, sizeof(*p));
		err = -EMSGSIZE;
		if (attr == NULL)
			goto out_free_skb;

		p = nla_data(attr);
	}

	copy_to_user_policy(xp, p, dir);
	err = xfrm_copy_to_user_tmpl(xp, skb);
	if (!err)
		err = copy_to_user_policy_type(xp->type, skb);
	if (!err)
		err = xfrm_mark_put(skb, &xp->mark);
	if (err)
		goto out_free_skb;

	nlmsg_end(skb, nlh);

	return xfrm_nlmsg_multicast(net, skb, 0, XFRMNLGRP_POLICY);

out_free_skb:
	kfree_skb(skb);
	return err;
}
