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
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/socket.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <net/sock.h>

#include "xsk.h"
#include "xsk_buff.h"
#include "xsk_ring.h"

#define XSK_UMEM_MIN_FRAME_SIZE 2048

struct xsk_info {
	struct xsk_queue *q;
	struct xsk_umem *umem;
	struct socket *mrsock;
	struct xsk_buff_info *buff_info;
};

struct xdp_sock {
	/* struct sock must be the first member of struct xdp_sock */
	struct sock sk;
	struct xsk_info rx;
	struct xsk_info tx;
	struct xsk_umem *umem;
};

static struct xdp_sock *xdp_sk(struct sock *sk)
{
	return (struct xdp_sock *)sk;
}

static void xsk_umem_unpin_pages(struct xsk_umem *umem)
{
	unsigned int i;

	if (umem->pgs) {
		for (i = 0; i < umem->npgs; i++) {
			struct page *page = umem->pgs[i];

			set_page_dirty_lock(page);
			put_page(page);
		}

		kfree(umem->pgs);
		umem->pgs = NULL;
	}
}

static void xsk_umem_destroy(struct xsk_umem *umem)
{
	struct mm_struct *mm;
	struct task_struct *task;
	unsigned long diff;

	if (!umem)
		return;

	xsk_umem_unpin_pages(umem);

	task = get_pid_task(umem->pid, PIDTYPE_PID);
	put_pid(umem->pid);
	if (!task)
		goto out;
	mm = get_task_mm(task);
	put_task_struct(task);
	if (!mm)
		goto out;

	diff = umem->size >> PAGE_SHIFT;

	down_write(&mm->mmap_sem);
	mm->pinned_vm -= diff;
	up_write(&mm->mmap_sem);
	mmput(mm);
out:
	kfree(umem);
}

static struct xsk_umem *xsk_umem_create(u64 addr, u64 size, u32 frame_size,
					u32 data_headroom)
{
	struct xsk_umem *umem;
	unsigned int nframes;
	int size_chk;

	if (frame_size < XSK_UMEM_MIN_FRAME_SIZE || frame_size > PAGE_SIZE) {
		/* Strictly speaking we could support this, if:
		 * - huge pages, or*
		 * - using an IOMMU, or
		 * - making sure the memory area is consecutive
		 * but for now, we simply say "computer says no".
		 */
		return ERR_PTR(-EINVAL);
	}

	if (!is_power_of_2(frame_size))
		return ERR_PTR(-EINVAL);

	if (!PAGE_ALIGNED(addr)) {
		/* Memory area has to be page size aligned. For
		 * simplicity, this might change.
		 */
		return ERR_PTR(-EINVAL);
	}

	if ((addr + size) < addr)
		return ERR_PTR(-EINVAL);

	nframes = size / frame_size;
	if (nframes == 0)
		return ERR_PTR(-EINVAL);

	data_headroom =	ALIGN(data_headroom, 64);

	size_chk = frame_size - data_headroom - XSK_KERNEL_HEADROOM;
	if (size_chk < 0)
		return ERR_PTR(-EINVAL);

	umem = kzalloc(sizeof(*umem), GFP_KERNEL);
	if (!umem)
		return ERR_PTR(-ENOMEM);

	umem->pid = get_task_pid(current, PIDTYPE_PID);
	umem->size = (size_t)size;
	umem->address = (unsigned long)addr;
	umem->frame_size = frame_size;
	umem->nframes = nframes;
	umem->data_headroom = data_headroom;
	umem->pgs = NULL;

	return umem;
}

static int xsk_umem_pin_pages(struct xsk_umem *umem)
{
	unsigned int gup_flags = FOLL_WRITE;
	long npgs;
	int err;

	/* XXX Fix so that we don't always pin.
	 * "copy to user" from interrupt context, but how?
	 */
	umem->pgs = kcalloc(umem->npgs, sizeof(*umem->pgs), GFP_ATOMIC);
	if (!umem->pgs)
		return -ENOMEM;

	npgs = get_user_pages(umem->address, umem->npgs,
			      gup_flags, &umem->pgs[0], NULL);
	if (npgs != umem->npgs) {
		if (npgs >= 0) {
			umem->npgs = npgs;
			err = -ENOMEM;
			goto out_pin;
		}
		err = npgs;
		goto out_pgs;
	}

	return 0;

out_pin:
	xsk_umem_unpin_pages(umem);
out_pgs:
	kfree(umem->pgs);
	umem->pgs = NULL;

	return err;
}

static struct xsk_umem *xsk_mem_reg(u64 addr, u64 size, u32 frame_size,
				    u32 data_headroom)
{
	unsigned long lock_limit, locked, npages;
	int ret = 0;
	struct xsk_umem *umem;

	if (!can_do_mlock())
		return ERR_PTR(-EPERM);

	umem = xsk_umem_create(addr, size, frame_size, data_headroom);
	if (IS_ERR(umem))
		return umem;

	npages = PAGE_ALIGN(umem->nframes * umem->frame_size) >> PAGE_SHIFT;

	down_write(&current->mm->mmap_sem);

	locked = npages + current->mm->pinned_vm;
	lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	if (locked > lock_limit && !capable(CAP_IPC_LOCK)) {
		ret = -ENOMEM;
		goto out;
	}

	if (npages == 0 || npages > UINT_MAX) {
		ret = -EINVAL;
		goto out;
	}
	umem->npgs = npages;

	ret = xsk_umem_pin_pages(umem);

out:
	if (ret < 0) {
		put_pid(umem->pid);
		kfree(umem);
	} else {
		current->mm->pinned_vm = locked;
	}

	up_write(&current->mm->mmap_sem);

	return ret < 0 ? ERR_PTR(ret) : umem;
}

static struct socket *xsk_umem_sock_get(int fd)
{
	struct socket *sock;
	int err;

	sock = sockfd_lookup(fd, &err);
	if (!sock)
		return ERR_PTR(err);

	/* Parameter checking */
	if (sock->sk->sk_family != PF_XDP) {
		err = -ESOCKTNOSUPPORT;
		goto out;
	}

	if (!xdp_sk(sock->sk)->umem) {
		err = -ESOCKTNOSUPPORT;
		goto out;
	}

	return sock;
out:
	sockfd_put(sock);
	return ERR_PTR(err);
}

static int xsk_init_ring(struct sock *sk, int mr_fd, u32 desc_nr,
			 struct xsk_info *info)
{
	struct xsk_umem *umem;
	struct socket *mrsock;

	if (desc_nr == 0)
		return -EINVAL;

	mrsock = xsk_umem_sock_get(mr_fd);
	if (IS_ERR(mrsock))
		return PTR_ERR(mrsock);
	umem = xdp_sk(mrsock->sk)->umem;

	/* Check if umem is from this socket, if so do not make
	 * circular references.
	 */
	lock_sock(sk);
	if (sk->sk_socket == mrsock)
		sockfd_put(mrsock);

	info->q = xskq_create(desc_nr);
	if (!info->q)
		goto out_queue;

	info->umem = umem;
	info->mrsock = mrsock;
	release_sock(sk);
	return 0;

out_queue:
	release_sock(sk);
	return -ENOMEM;
}

static int xsk_init_rx_ring(struct sock *sk, int mr_fd, u32 desc_nr)
{
	struct xdp_sock *xs = xdp_sk(sk);

	return xsk_init_ring(sk, mr_fd, desc_nr, &xs->rx);
}

static int xsk_init_tx_ring(struct sock *sk, int mr_fd, u32 desc_nr)
{
	struct xdp_sock *xs = xdp_sk(sk);

	return xsk_init_ring(sk, mr_fd, desc_nr, &xs->tx);
}

static int xsk_release(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	struct net *net;

	if (!sk)
		return 0;

	net = sock_net(sk);

	local_bh_disable();
	sock_prot_inuse_add(net, sk->sk_prot, -1);
	local_bh_enable();

	xsk_umem_destroy(xs->umem);
	xskq_destroy(xs->rx.q);
	xskq_destroy(xs->tx.q);

	sock_orphan(sk);
	sock->sk = NULL;

	sk_refcnt_debug_release(sk);
	sock_put(sk);

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
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);

	if (level != SOL_XDP)
		return -ENOPROTOOPT;

	switch (optname) {
	case XDP_MEM_REG:
	{
		struct xdp_mr_req req;
		struct xsk_umem *umem;

		if (optlen < sizeof(req))
			return -EINVAL;
		if (copy_from_user(&req, optval, sizeof(req)))
			return -EFAULT;

		umem = xsk_mem_reg(req.addr, req.len, req.frame_size,
				   req.data_headroom);
		if (IS_ERR(umem))
			return PTR_ERR(umem);

		lock_sock(sk);
		if (xs->umem) { /* XXX create and check afterwards... really? */
			release_sock(sk);
			xsk_umem_destroy(umem);
			return -EBUSY;
		}
		xs->umem = umem;
		release_sock(sk);

		return 0;
	}
	case XDP_RX_RING:
	case XDP_TX_RING:
	{
		struct xdp_ring_req req;

		if (optlen < sizeof(req))
			return -EINVAL;
		if (copy_from_user(&req, optval, sizeof(req)))
			return -EFAULT;

		if (optname == XDP_TX_RING)
			return xsk_init_tx_ring(sk, req.mr_fd, req.desc_nr);

		return xsk_init_rx_ring(sk, req.mr_fd, req.desc_nr);
	}
	default:
		break;
	}

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
	unsigned long size = vma->vm_end - vma->vm_start;
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	struct xsk_queue *q;
	unsigned long pfn;

	if (vma->vm_pgoff == XDP_PGOFF_RX_RING)
		q = xs->rx.q;
	else if (vma->vm_pgoff == XDP_PGOFF_TX_RING >> PAGE_SHIFT)
		q = xs->tx.q;
	else
		return -EINVAL;

	if (size != xskq_get_ring_size(q))
		return -EFBIG;

	pfn = virt_to_phys(xskq_get_ring_address(q)) >> PAGE_SHIFT;
	return remap_pfn_range(vma, vma->vm_start, pfn,
			       size, vma->vm_page_prot);
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

static void xsk_destruct(struct sock *sk)
{
	if (!sock_flag(sk, SOCK_DEAD))
		return;

	sk_refcnt_debug_dec(sk);
}

static int xsk_create(struct net *net, struct socket *sock, int protocol,
		      int kern)
{
	struct sock *sk;

	if (!ns_capable(net->user_ns, CAP_NET_RAW))
		return -EPERM;
	if (sock->type != SOCK_RAW)
		return -ESOCKTNOSUPPORT;

	/* XXX Require ETH_P_IP? Something else? */
	if (protocol)
		return -EPROTONOSUPPORT;

	sock->state = SS_UNCONNECTED;

	sk = sk_alloc(net, PF_XDP, GFP_KERNEL, &xsk_proto, kern);
	if (!sk)
		return -ENOBUFS;

	sock->ops = &xsk_proto_ops;

	sock_init_data(sock, sk);

	sk->sk_family = PF_XDP;

	sk->sk_destruct = xsk_destruct;
	sk_refcnt_debug_inc(sk);

	local_bh_disable();
	sock_prot_inuse_add(net, &xsk_proto, 1);
	local_bh_enable();

	return 0;
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
