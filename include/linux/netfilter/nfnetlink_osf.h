#ifndef _NFNETLINK_OSF_H
#define _NFNETLINK_OSF_H

#include <linux/list.h>

#include <linux/netfilter/nfnetlink.h>

extern struct list_head nf_osf_fingers[2];

int nf_osf_add_callback(struct net *net, struct sock *ctnl,
			struct sk_buff *skb, const struct nlmsghdr *nlh,
			const struct nlattr * const osf_attrs[],
			struct netlink_ext_ack *extack);

int nf_osf_remove_callback(struct net *net, struct sock *ctnl,
			   struct sk_buff *skb, const struct nlmsghdr *nlh,
			   const struct nlattr * const osf_attrs[],
			   struct netlink_ext_ack *extack);

#endif	/* _NFNETLINK_OSF_H */
