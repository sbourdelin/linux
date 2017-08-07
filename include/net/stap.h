/*
 * Socket tap
 *
 * Copyright (c) 2017 Tom Herbert <tom@quantonium.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#ifndef __NET_STAP_H_
#define __NET_STAP_H_

#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/strparser.h>

struct stap_bops {
	struct strparser strp;
	struct bpf_prog *parse_prog;
	struct bpf_prog *verdict_prog;
};

struct stap_sock {
	struct sock *sk; /* Associated socket */

	const struct proto_ops *orig_ops;

	void (*save_data_ready)(struct sock *sk);
	void (*save_write_space)(struct sock *sk);
	void (*save_state_change)(struct sock *sk);

	/* Send items */
	struct stap_bops send_bops;
	struct sk_buff_head build_list;
	struct sk_buff_head ready_list;

	/* Receive items */
	struct stap_bops recv_bops;
	struct sk_buff *recv_skb;
};

#endif /* __NET_STAP_H_ */
