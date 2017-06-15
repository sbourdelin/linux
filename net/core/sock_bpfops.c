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
static struct bpf_prog *bpf_socket_ops_prog;
static DEFINE_RWLOCK(bpf_socket_ops_lock);

int bpf_socket_ops_set_prog(int fd)
{
	int err = 0;

	write_lock(&bpf_socket_ops_lock);
	if (bpf_socket_ops_prog) {
		bpf_prog_put(bpf_socket_ops_prog);
		bpf_socket_ops_prog = NULL;
	}

	/* fd of zero is used as a signal to remove the current
	 * bpf_socket_ops_prog.
	 */
	if (fd == 0) {
		write_unlock(&bpf_socket_ops_lock);
		return 1;
	}

	bpf_socket_ops_prog = bpf_prog_get_type(fd, BPF_PROG_TYPE_SOCKET_OPS);
	if (IS_ERR(bpf_socket_ops_prog)) {
		bpf_prog_put(bpf_socket_ops_prog);
		bpf_socket_ops_prog = NULL;
		err = 1;
	}
	write_unlock(&bpf_socket_ops_lock);
	return err;
}

int bpf_socket_ops_call(struct bpf_socket_ops_kern *bpf_socket)
{
	int ret;

	read_lock(&bpf_socket_ops_lock);
	if (bpf_socket_ops_prog) {
		rcu_read_lock();
		ret = (int)BPF_PROG_RUN(bpf_socket_ops_prog, bpf_socket);
		rcu_read_unlock();
	} else {
		ret = -1;
	}
	read_unlock(&bpf_socket_ops_lock);
	return ret;
}
