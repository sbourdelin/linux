/*
 * BPF support for sockets
 *
 * Copyright (c) 2016 Lawrence Brakmo <brakmo@fb.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#include <net/sock.h>
#include <linux/skbuff.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/errno.h>
#ifdef CONFIG_NET_NS
#include <net/net_namespace.h>
#include <linux/proc_ns.h>
#endif

/* Global BPF program for sockets */
static struct bpf_prog *bpf_global_sock_ops_prog;

int bpf_sock_ops_detach_global_prog(void)
{
	struct bpf_prog *old_prog;

	old_prog = xchg(&bpf_global_sock_ops_prog, NULL);

	if (old_prog)
		bpf_prog_put(old_prog);

	return 0;
}

int bpf_sock_ops_attach_global_prog(int fd)
{
	struct bpf_prog *prog, *old_prog;
	int err = 0;

	prog = bpf_prog_get_type(fd, BPF_PROG_TYPE_SOCK_OPS);
	if (IS_ERR(prog))
		return PTR_ERR(prog);

	old_prog = xchg(&bpf_global_sock_ops_prog, prog);
	if (old_prog)
		bpf_prog_put(old_prog);
	return err;
}

int bpf_sock_ops_call(struct bpf_sock_ops_kern *bpf_sock)
{
	struct bpf_prog *prog;
	int ret;

	rcu_read_lock();
	prog =  READ_ONCE(bpf_global_sock_ops_prog);
	if (prog)
		ret = BPF_PROG_RUN(prog, bpf_sock);
	else
		ret = -1;
	rcu_read_unlock();

	return ret;
}
