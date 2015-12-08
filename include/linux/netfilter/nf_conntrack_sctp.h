#ifndef _NF_CONNTRACK_SCTP_H
#define _NF_CONNTRACK_SCTP_H
/* SCTP tracking. */

#include <uapi/linux/netfilter/nf_conntrack_sctp.h>
#include <linux/rhashtable.h>

struct nf_conn;
struct sctp_vtaghash_node {
	struct rhash_head node;
	__be16 sport;
	__be16 dport;
	__be32 vtag;
	struct net *net;
	int dir;
	atomic_t count;
	struct rcu_head rcu_head;
};

struct ip_ct_sctp {
	enum sctp_conntrack state;

	__be32 vtag[IP_CT_DIR_MAX];

	struct sctp_vtaghash_node *vtagnode[IP_CT_DIR_MAX];
	bool crossed;
	bool from_heartbeat;
};

#endif /* _NF_CONNTRACK_SCTP_H */
