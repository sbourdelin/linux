/*
 * XDP sockets
 *
 * AF_XDP sockets allows a channel between XDP programs and userspace
 * applications.
 *
 * Copyright(c) 2017 Intel Corporation.
 *
 * Author(s): Björn Töpel <bjorn.topel@intel.com>
 *	      Magnus Karlsson <magnus.karlsson@intel.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#define pr_fmt(fmt) "AF_XDP: %s: " fmt, __func__

#include <linux/if_xdp.h>
#include <linux/init.h>
#include <linux/socket.h>
#include <net/sock.h>

#include "xsk.h"

struct xdp_sock {
	/* struct sock must be the first member of struct xdp_sock */
	struct sock sk;
};

static int xsk_release(struct socket *sock)
{
	return 0;
}

static int xsk_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	return -EOPNOTSUPP;
}

static unsigned int xsk_poll(struct file *file, struct socket *sock,
			     struct poll_table_struct *wait)
{
	return -EOPNOTSUPP;
}

static int xsk_setsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, unsigned int optlen)
{
	return -ENOPROTOOPT;
}

static int xsk_getsockopt(struct socket *sock, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	return -EOPNOTSUPP;
}

static int xsk_sendmsg(struct socket *sock, struct msghdr *m, size_t total_len)
{
	return -EOPNOTSUPP;
}

static int xsk_mmap(struct file *file, struct socket *sock,
		    struct vm_area_struct *vma)
{
	return -EOPNOTSUPP;
}

static struct proto xsk_proto = {
	.name =		"XDP",
	.owner =	THIS_MODULE,
	.obj_size =	sizeof(struct xdp_sock),
};

static const struct proto_ops xsk_proto_ops = {
	.family =	PF_XDP,
	.owner =	THIS_MODULE,
	.release =	xsk_release,
	.bind =		xsk_bind,
	.connect =	sock_no_connect,
	.socketpair =	sock_no_socketpair,
	.accept =	sock_no_accept,
	.getname =	sock_no_getname, /* XXX do we need this? */
	.poll =		xsk_poll,
	.ioctl =	sock_no_ioctl, /* XXX do we need this? */
	.listen =	sock_no_listen,
	.shutdown =	sock_no_shutdown,
	.setsockopt =	xsk_setsockopt,
	.getsockopt =	xsk_getsockopt,
	/* XXX make sure we don't rely on any ioctl/{get,set}sockopt that would require CONFIG_COMPAT! */
	.sendmsg =	xsk_sendmsg,
	.recvmsg =	sock_no_recvmsg,
	.mmap =		xsk_mmap,
	.sendpage =	sock_no_sendpage,
	/* the rest vvv, OK to be missing implementation -- checked against NULL. */
};

static int xsk_create(struct net *net, struct socket *sock, int protocol,
		      int kern)
{
	return -EOPNOTSUPP;
}

static const struct net_proto_family xsk_family_ops = {
	.family = PF_XDP,
	.create = xsk_create,
	.owner	= THIS_MODULE,
};

/* XXX Do we need any namespace support? _pernet_subsys and friends */
static int __init xsk_init(void)
{
	int err;

	err = proto_register(&xsk_proto, 0 /* no slab */);
	if (err)
		goto out;

	err = sock_register(&xsk_family_ops);
	if (err)
		goto out_proto;

	return 0;

out_proto:
	proto_unregister(&xsk_proto);
out:
	return err;
}

fs_initcall(xsk_init);
