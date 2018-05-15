/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _NF_NAT_CORE_H
#define _NF_NAT_CORE_H
#include <linux/list.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/netfilter/nf_nat.h>

/* This header used to share core functionality between the standalone
   NAT module, and the compatibility layer's use of NAT for masquerading. */

unsigned int nf_nat_packet(struct nf_conn *ct, enum ip_conntrack_info ctinfo,
			   unsigned int hooknum, struct sk_buff *skb);

int nf_xfrm_me_harder(struct net *net, struct sk_buff *skb, unsigned int family);

static inline int nf_nat_initialized(struct nf_conn *ct,
				     enum nf_nat_manip_type manip)
{
	if (manip == NF_NAT_MANIP_SRC)
		return ct->status & IPS_SRC_NAT_DONE;
	else
		return ct->status & IPS_DST_NAT_DONE;
}

struct nlattr;

#include <net/flow.h>

struct nf_nat_hook {
	int (*parse_nat_setup)(struct nf_conn *ct, enum nf_nat_manip_type manip,
			       const struct nlattr *attr);
	void (*decode_session)(struct sk_buff *skb, struct flowi *fl);
	unsigned int (*manip_pkt)(struct sk_buff *skb, struct nf_conn *ct,
				  enum nf_nat_manip_type mtype,
				  enum ip_conntrack_dir dir);
};

extern struct nf_nat_hook __rcu *nf_nat_hook;

static inline void
nf_nat_decode_session(struct sk_buff *skb, struct flowi *fl, u_int8_t family)
{
#ifdef CONFIG_NF_NAT_NEEDED
	struct nf_nat_hook *nat_hook;

	rcu_read_lock();
	nat_hook = rcu_dereference(nf_nat_hook);
	if (nat_hook->decode_session)
		nat_hook->decode_session(skb, fl);
	rcu_read_unlock();
#endif
}

#endif /* _NF_NAT_CORE_H */
