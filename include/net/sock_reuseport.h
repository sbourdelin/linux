/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SOCK_REUSEPORT_H
#define _SOCK_REUSEPORT_H

#include <linux/filter.h>
#include <linux/skbuff.h>
#include <linux/types.h>
#include <net/sock.h>

struct sock_reuseport {
	struct rcu_head		rcu;

	u16			max_socks;	/* length of socks */
	u16			num_socks;	/* elements in socks */
	struct bpf_prog __rcu	*prog;		/* optional BPF sock selector */
	struct sock		*socks[0];	/* array of sock pointers */
};

struct reuseport_info {
	struct sock_reuseport *reuse;
	struct sock *sk;
	u16 socks;
};

extern int reuseport_alloc(struct sock *sk);
extern int reuseport_add_sock(struct sock *sk, struct sock *sk2);
extern void reuseport_detach_sock(struct sock *sk);
bool __reuseport_get_info(struct sock *sk, struct sk_buff *skb, int hdr_len,
			  struct reuseport_info *info);
static inline struct sock *__reuseport_select_sock(struct reuseport_info *info,
						   u32 hash)
{
	return info->reuse->socks[reciprocal_scale(hash, info->socks)];
}

#define reuseport_select_sock(sk, skb, net, hlen, fn, saddr, sport, daddr, dport) \
({									      \
	struct reuseport_info info;					      \
	info.sk = NULL;							      \
	if (sk->sk_reuseport) {						      \
		rcu_read_lock();					      \
		if (__reuseport_get_info(sk, skb, hlen, &info) && !info.sk)   \
			info.sk = __reuseport_select_sock(&info,	      \
					 fn(net, daddr, hnum, saddr, sport)); \
		rcu_read_unlock();					      \
	}								      \
	info.sk;							      \
})

extern struct bpf_prog *reuseport_attach_prog(struct sock *sk,
					      struct bpf_prog *prog);

#endif  /* _SOCK_REUSEPORT_H */
