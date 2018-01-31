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
#define XSK_ARRAY_SIZE 512

struct xsk_info {
	struct xsk_packet_array *pa;
	spinlock_t pa_lock;
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
	struct net_device *dev;
	struct xsk_umem *umem;
	/* Protects multiple processes from entering sendmsg */
	struct mutex tx_mutex;
	u32 ifindex;
	u16 queue_id;
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

	if (xs->dev) {
		struct xdp_sock *xs_prev;

		xs_prev = xs->dev->_rx[xs->queue_id].xs;
		rcu_assign_pointer(xs->dev->_rx[xs->queue_id].xs, NULL);

		/* Wait for driver to stop using the xdp socket. */
		synchronize_net();

		xskpa_destroy(xs->rx.pa);
		xskpa_destroy(xs->tx.pa);
		xsk_umem_destroy(xs_prev->umem);
		xskq_destroy(xs_prev->rx.q);
		xskq_destroy(xs_prev->tx.q);
		kobject_put(&xs_prev->dev->_rx[xs->queue_id].kobj);
		dev_put(xs_prev->dev);
	}

	sock_orphan(sk);
	sock->sk = NULL;

	sk_refcnt_debug_release(sk);
	sock_put(sk);

	return 0;
}

static int xsk_bind(struct socket *sock, struct sockaddr *addr, int addr_len)
{
	struct sockaddr_xdp *sxdp = (struct sockaddr_xdp *)addr;
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);
	struct net_device *dev_curr;
	struct net_device *dev;
	int err = 0;

	if (addr_len < sizeof(struct sockaddr_xdp))
		return -EINVAL;
	if (sxdp->sxdp_family != AF_XDP)
		return -EINVAL;

	lock_sock(sk);
	dev_curr = xs->dev;
	dev = dev_get_by_index_rcu(sock_net(sk), sxdp->sxdp_ifindex);
	if (!dev) {
		err = -ENODEV;
		goto out_unlock;
	}
	dev_hold(dev);

	if (dev_curr && dev_curr != dev) {
		/* XXX Needs rebind code here */
		err = -EBUSY;
		goto out_unlock;
	}

	if (!xs->rx.q || !xs->tx.q) {
		/* XXX For now require Tx and Rx */
		err = -EINVAL;
		goto out_unlock;
	}

	if (sxdp->sxdp_queue_id > dev->num_rx_queues) {
		err = -EINVAL;
		goto out_unlock;
	}
	kobject_get(&dev->_rx[sxdp->sxdp_queue_id].kobj);

	xs->dev = dev;
	xs->ifindex = sxdp->sxdp_ifindex;
	xs->queue_id = sxdp->sxdp_queue_id;
	spin_lock_init(&xs->rx.pa_lock);
	spin_lock_init(&xs->tx.pa_lock);
	mutex_init(&xs->tx_mutex);

	/* Rx */
	xs->rx.buff_info = xsk_buff_info_create(xs->rx.umem);
	if (!xs->rx.buff_info) {
		err = -ENOMEM;
		goto out_unlock;
	}
	xskq_set_buff_info(xs->rx.q, xs->rx.buff_info, XSK_VALIDATION_RX);

	/* Rx packet array is used for copy semantics... */
	xs->rx.pa = xskpa_create((struct xsk_user_queue *)xs->rx.q,
				 xs->rx.buff_info, XSK_ARRAY_SIZE);
	if (!xs->rx.pa) {
		err = -ENOMEM;
		goto out_rx_pa;
	}

	/* Tx */
	xs->tx.buff_info = xsk_buff_info_create(xs->tx.umem);
	if (!xs->tx.buff_info) {
		err = -ENOMEM;
		goto out_tx_bi;
	}
	xskq_set_buff_info(xs->tx.q, xs->tx.buff_info, XSK_VALIDATION_TX);

	xs->tx.pa = xskpa_create((struct xsk_user_queue *)xs->tx.q,
				 xs->tx.buff_info, XSK_ARRAY_SIZE);
	if (!xs->tx.pa) {
		err = -ENOMEM;
		goto out_tx_pa;
	}

	rcu_assign_pointer(dev->_rx[sxdp->sxdp_queue_id].xs, xs);

	goto out_unlock;

out_tx_pa:
	xsk_buff_info_destroy(xs->tx.buff_info);
	xs->tx.buff_info = NULL;
out_tx_bi:
	xskpa_destroy(xs->rx.pa);
	xs->rx.pa = NULL;
out_rx_pa:
	xsk_buff_info_destroy(xs->rx.buff_info);
	xs->rx.buff_info = NULL;
out_unlock:
	if (err)
		dev_put(dev);
	release_sock(sk);
	if (dev_curr)
		dev_put(dev_curr);
	return err;
}

static inline struct xdp_sock *lookup_xsk(struct net_device *dev,
					  unsigned int queue_id)
{
	if (unlikely(queue_id > dev->num_rx_queues))
		return NULL;

	return rcu_dereference(dev->_rx[queue_id].xs);
}

int xsk_generic_rcv(struct xdp_buff *xdp)
{
	u32 len = xdp->data_end - xdp->data;
	struct xsk_frame_set p;
	struct xdp_sock *xsk;
	bool ok;

	rcu_read_lock();
	xsk = lookup_xsk(xdp->rxq->dev, xdp->rxq->queue_index);
	if (unlikely(!xsk)) {
		rcu_read_unlock();
		return -EINVAL;
	}

	spin_lock(&xsk->rx.pa_lock);
	ok = xskpa_next_frame_populate(xsk->rx.pa, &p);
	spin_unlock(&xsk->rx.pa_lock);

	if (!ok) {
		rcu_read_unlock();
		return -ENOSPC;
	}

	memcpy(xskf_get_data(&p), xdp->data, len);
	xskf_set_frame_no_offset(&p, len, true);
	spin_lock(&xsk->rx.pa_lock);
	xskpa_flush(xsk->rx.pa);
	spin_unlock(&xsk->rx.pa_lock);
	rcu_read_unlock();

	return 0;
}
EXPORT_SYMBOL_GPL(xsk_generic_rcv);

struct xdp_sock *xsk_rcv(struct xdp_sock *xsk, struct xdp_buff *xdp)
{
	u32 len = xdp->data_end - xdp->data;
	struct xsk_frame_set p;

	rcu_read_lock();
	if (!xsk)
		xsk = lookup_xsk(xdp->rxq->dev, xdp->rxq->queue_index);
	if (unlikely(!xsk)) {
		rcu_read_unlock();
		return ERR_PTR(-EINVAL);
	}

	if (!xskpa_next_frame_populate(xsk->rx.pa, &p)) {
		rcu_read_unlock();
		return ERR_PTR(-ENOSPC);
	}

	memcpy(xskf_get_data(&p), xdp->data, len);
	xskf_set_frame_no_offset(&p, len, true);
	rcu_read_unlock();

	/* We assume that the semantic of xdp_do_redirect is such that
	 * ndo_xdp_xmit will decrease the refcount of the page when it
	 * is done with the page. Thus, if we want to guarantee the
	 * existence of the page in the calling driver, we need to
	 * bump the refcount. Unclear what the correct semantic is
	 * supposed to be.
	 */
	page_frag_free(xdp->data);

	return xsk;
}
EXPORT_SYMBOL_GPL(xsk_rcv);

int xsk_zc_rcv(struct xdp_sock *xsk, struct xdp_buff *xdp)
{
	u32 offset = xdp->data - xdp->data_hard_start;
	u32 len = xdp->data_end - xdp->data;
	struct xsk_frame_set p;

	/* We do not need any locking here since we are guaranteed
	 * a single producer and a single consumer.
	 */
	if (xskpa_next_frame_populate(xsk->rx.pa, &p)) {
		xskf_set_frame(&p, len, offset, true);
		return 0;
	}

	/* No user-space buffer to put the packet in. */
	return -ENOSPC;
}
EXPORT_SYMBOL_GPL(xsk_zc_rcv);

void xsk_flush(struct xdp_sock *xsk)
{
	rcu_read_lock();
	if (!xsk)
		xsk = lookup_xsk(xsk->dev, xsk->queue_id);
	if (unlikely(!xsk)) {
		rcu_read_unlock();
		return;
	}

	WARN_ON_ONCE(xskpa_flush(xsk->rx.pa));
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(xsk_flush);

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

void xsk_tx_completion(struct net_device *dev, u16 queue_index,
		       unsigned int npackets)
{
	unsigned long flags;
	struct xdp_sock *xs;

	rcu_read_lock();
	xs = lookup_xsk(dev, queue_index);
	if (unlikely(!xs)) {
		rcu_read_unlock();
		return;
	}

	spin_lock_irqsave(&xs->tx.pa_lock, flags);
	WARN_ON_ONCE(xskpa_flush_n(xs->tx.pa, npackets));
	spin_unlock_irqrestore(&xs->tx.pa_lock, flags);
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(xsk_tx_completion);

static void xsk_destruct_skb(struct sk_buff *skb)
{
	u64 idx = (u64)skb_shinfo(skb)->destructor_arg;
	struct xsk_frame_set p = {.start = idx,
				  .curr = idx,
				  .end = idx + 1};
	struct xdp_sock *xs;
	unsigned long flags;

	rcu_read_lock();
	xs = lookup_xsk(skb->dev, skb_get_queue_mapping(skb));
	if (unlikely(!xs)) {
		rcu_read_unlock();
		return;
	}

	p.pkt_arr = xs->tx.pa;
	xskf_packet_completed(&p);
	spin_lock_irqsave(&xs->tx.pa_lock, flags);
	WARN_ON_ONCE(xskpa_flush_completed(xs->tx.pa));
	spin_unlock_irqrestore(&xs->tx.pa_lock, flags);
	rcu_read_unlock();

	sock_wfree(skb);
}

static int xsk_xmit_skb(struct sk_buff *skb)
{
	struct net_device *dev = skb->dev;
	struct sk_buff *orig_skb = skb;
	struct netdev_queue *txq;
	int ret = NETDEV_TX_BUSY;
	bool again = false;

	if (unlikely(!netif_running(dev) || !netif_carrier_ok(dev)))
		goto drop;

	skb = validate_xmit_skb_list(skb, dev, &again);
	if (skb != orig_skb)
		return NET_XMIT_DROP;

	txq = skb_get_tx_queue(dev, skb);

	local_bh_disable();

	HARD_TX_LOCK(dev, txq, smp_processor_id());
	if (!netif_xmit_frozen_or_drv_stopped(txq))
		ret = netdev_start_xmit(skb, dev, txq, false);
	HARD_TX_UNLOCK(dev, txq);

	local_bh_enable();

	if (!dev_xmit_complete(ret))
		goto out_err;

	return ret;
drop:
	atomic_long_inc(&dev->tx_dropped);
out_err:
	kfree_skb(skb);
	return NET_XMIT_DROP;
}

static int xsk_generic_xmit(struct sock *sk, struct msghdr *m,
			    size_t total_len)
{
	bool need_wait = !(m->msg_flags & MSG_DONTWAIT);
	struct xdp_sock *xs = xdp_sk(sk);
	struct xsk_frame_set p;
	struct sk_buff *skb;
	unsigned long flags;
	int err = 0;

	if (need_wait)
		/* Not implemented yet. */
		return -EINVAL;

	mutex_lock(&xs->tx_mutex);
	spin_lock_irqsave(&xs->tx.pa_lock, flags);
	xskpa_populate(xs->tx.pa);
	spin_unlock_irqrestore(&xs->tx.pa_lock, flags);

	while (xskpa_next_packet(xs->tx.pa, &p)) {
		u32 len = xskf_get_packet_len(&p);

		if (unlikely(len > xs->dev->mtu)) {
			err = -EMSGSIZE;
			goto out_err;
		}

		skb = sock_alloc_send_skb(sk, len, !need_wait, &err);
		if (unlikely(!skb)) {
			err = -EAGAIN;
			goto out_err;
		}

		/* XXX Use fragments for the data here */
		skb_put(skb, len);
		err = skb_store_bits(skb, 0, xskf_get_data(&p), len);
		if (unlikely(err))
			goto out_skb;

		skb->dev = xs->dev;
		skb->priority = sk->sk_priority;
		skb->mark = sk->sk_mark;
		skb_set_queue_mapping(skb, xs->queue_id);
		skb_shinfo(skb)->destructor_arg =
			(void *)(long)xskf_get_frame_id(&p);
		skb->destructor = xsk_destruct_skb;

		err = xsk_xmit_skb(skb);
		/* Ignore NET_XMIT_CN as packet might have been sent */
		if (err == NET_XMIT_DROP || err == NETDEV_TX_BUSY) {
			err = -EAGAIN;
			break;
		}
	}

	mutex_unlock(&xs->tx_mutex);
	return err;

out_skb:
	kfree_skb(skb);
out_err:
	xskf_set_error(&p, -err);
	xskf_packet_completed(&p);
	spin_lock_irqsave(&xs->tx.pa_lock, flags);
	WARN_ON_ONCE(xskpa_flush_completed(xs->tx.pa));
	spin_unlock_irqrestore(&xs->tx.pa_lock, flags);
	mutex_unlock(&xs->tx_mutex);

	return err;
}

static int xsk_sendmsg(struct socket *sock, struct msghdr *m, size_t total_len)
{
	struct sock *sk = sock->sk;
	struct xdp_sock *xs = xdp_sk(sk);

	if (unlikely(!xs->dev))
		return -ENXIO;
	if (unlikely(!(xs->dev->flags & IFF_UP)))
		return -ENETDOWN;

	return xsk_generic_xmit(sk, m, total_len);
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
