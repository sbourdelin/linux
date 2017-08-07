/*
 * Socket tap
 *
 * Copyright (c) 2017 Tom Herbert <tom@quantonium.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 */

#include <linux/in.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched/signal.h>
#include <net/inet_common.h>
#include <net/inet_connection_sock.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/stap.h>
#include <uapi/linux/stap.h>

static struct proto_ops stap_tcp_stream_ops;

static inline struct stap_sock *tsk_from_socket(struct socket *sock)
{
	return (struct stap_sock *)sock->sk->sk_ulp_data;
}

static void stap_push(struct stap_sock *tsk);

static void stap_write_space(struct sock *sk)
{
	struct stap_sock *tsk = (struct stap_sock *)sk->sk_user_data;

	if (unlikely(!tsk))
		return;

	stap_push(tsk);
	tsk->save_write_space(sk);
}

static void stap_data_ready(struct sock *sk)
{
	struct stap_sock *tsk = (struct stap_sock *)sk->sk_user_data;

	if (unlikely(!tsk))
		return;

	strp_data_ready(&tsk->recv_bops.strp);
}

static void stap_state_change(struct sock *sk)
{
	struct stap_sock *tsk = (struct stap_sock *)sk->sk_user_data;

	if (unlikely(!tsk))
		return;

	tsk->save_state_change(sk);
}

/* Try to send completed message from construct queue to transport socket. */
static void stap_push(struct stap_sock *tsk)
{
	struct strp_msg *stm;
	struct sk_buff *skb;
	int n;

	while ((skb = skb_peek(&tsk->ready_list))) {
		stm = strp_msg(skb);
		WARN_ON(skb->len - stm->offset > stm->full_len);
		n = skb_send_sock_locked(tsk->sk, skb, stm->offset,
					 stm->full_len);
		if (n <= 0)
			return;

		stm->full_len -= n;
		stm->offset += n;
		tsk->sk->sk_wmem_queued -= n;
		sk_mem_uncharge(tsk->sk, n);

		if (!stm->full_len) {
			__skb_unlink(skb, &tsk->ready_list);
			kfree_skb(skb);
		}
	}
}

/* Process data pending from sendmsg */
static void stap_run(struct stap_sock *tsk)
{
	struct strparser *strp = &tsk->send_bops.strp;
	int offset, slen, eaten;
	struct strp_msg *stm;
	struct sk_buff *skb;

	while ((skb = skb_peek(&tsk->build_list))) {
		stm = strp_msg(skb);
		offset = stm->offset;
		slen = skb->len - offset;

		eaten = strp_process(strp, skb, offset, slen,
				     tsk->sk->sk_sndbuf, tsk->sk->sk_sndtimeo);
		if (eaten >= slen) {
			__skb_unlink(skb, &tsk->build_list);
			kfree_skb(skb);
		} else {
			stm->offset += offset;
		}
	}

	stap_push(tsk);
}

static int stap_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	struct sock *sk = sock->sk;
	long timeo = sock_sndtimeo(sk, msg->msg_flags & MSG_DONTWAIT);
	struct stap_sock *tsk = tsk_from_socket(sock);
	int flags = msg->msg_flags;
	struct sk_buff *skb;
	int copied = 0;
	int err;
	int copy;

	lock_sock(sk);

	err = -EPIPE;
	if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
		goto do_error;

	sk_clear_bit(SOCKWQ_ASYNC_NOSPACE, sk);

	while (msg_data_left(msg)) {
		struct page_frag *pfrag = sk_page_frag(sk);
		bool merge = true;
		int i;

		skb = skb_peek(&tsk->build_list);
		if (!skb)
			goto new_skb;

		i = skb_shinfo(skb)->nr_frags;

		if (!sk_page_frag_refill(sk, pfrag))
			goto wait_for_memory;

		if (!skb_can_coalesce(skb, i, pfrag->page,
				      pfrag->offset)) {
			if (i == MAX_SKB_FRAGS) {
new_skb:
				skb = alloc_skb(0, sk->sk_allocation);
				if (!skb)
					goto wait_for_memory;

				__skb_queue_tail(&tsk->build_list, skb);

				skb->ip_summed = CHECKSUM_UNNECESSARY;
				continue;
			}
			merge = false;
		}

		copy = min_t(int, msg_data_left(msg),
			     pfrag->size - pfrag->offset);

		if (!sk_wmem_schedule(sk, copy))
			goto wait_for_memory;

		err = skb_copy_to_page_nocache(sk, &msg->msg_iter, skb,
					       pfrag->page,
					       pfrag->offset,
					       copy);
		if (err)
			goto do_error;

		/* Update the skb. */
		if (merge) {
			skb_frag_size_add(&skb_shinfo(skb)->frags[i - 1], copy);
		} else {
			skb_fill_page_desc(skb, i, pfrag->page,
					   pfrag->offset, copy);
			get_page(pfrag->page);
		}

		pfrag->offset += copy;
		copied += copy;

		continue;

wait_for_memory:
		if (copied)
			stap_run(tsk);

		err = sk_stream_wait_memory(sk, &timeo);
		if (err != 0)
			goto do_error;
	}

out:
	if (copied)
		stap_run(tsk);

	release_sock(sk);
	return copied;

do_error:
	if (copied)
		goto out;

	err = sk_stream_error(sk, flags, err);

	release_sock(sk);
	return err;
}

static ssize_t stap_sendpage(struct socket *sock, struct page *page,
			     int offset, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	long timeo = sock_sndtimeo(sk, flags & MSG_DONTWAIT);
	struct stap_sock *tsk = tsk_from_socket(sock);
	size_t copied = 0;
	int err;

	if (unlikely(!tsk))
		return 0;

	lock_sock(sk);

	err = -EPIPE;
	if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
		goto do_error;

	sk_clear_bit(SOCKWQ_ASYNC_NOSPACE, sk);

	while (size) {
		struct sk_buff *skb;
		int copy, i;
		bool can_coalesce;

		copy = size;

		skb = skb_peek(&tsk->build_list);
		if (!skb) {
new_skb:
			skb = alloc_skb(0, sk->sk_allocation);
			if (!skb)
				goto wait_for_memory;

			__skb_queue_tail(&tsk->build_list, skb);

			skb->ip_summed = CHECKSUM_UNNECESSARY;
				continue;
		}

		i = skb_shinfo(skb)->nr_frags;
		can_coalesce = skb_can_coalesce(skb, i, page, offset);

		if (!can_coalesce && i >= sysctl_max_skb_frags)
			goto new_skb;

		if (!sk_wmem_schedule(sk, copy))
			goto wait_for_memory;

		if (can_coalesce) {
			skb_frag_size_add(&skb_shinfo(skb)->frags[i - 1],
					  copy);
		} else {
			get_page(page);
			skb_fill_page_desc(skb, i, page, offset, copy);
		}

		skb_shinfo(skb)->tx_flags |= SKBTX_SHARED_FRAG;

		skb->len += copy;
		skb->data_len += copy;
		skb->truesize += copy;
		sk->sk_wmem_queued += copy;
		sk_mem_charge(sk, copy);
		copied += copy;
		offset += copy;
		size -= copy;

		continue;

wait_for_memory:
		if (copied)
			stap_run(tsk);

		err = sk_stream_wait_memory(sk, &timeo);
		if (err != 0)
			goto do_error;
	}

out:
	if (copied)
		stap_run(tsk);

	release_sock(sk);
	return copied;

do_error:
	if (copied)
		goto out;

	err = sk_stream_error(sk, flags, err);

	release_sock(sk);
	return err;
}

static int stap_parse_send_strparser(struct strparser *strp,
				     struct sk_buff *skb)
{
	struct stap_sock *tsk = container_of(strp, struct stap_sock,
					       send_bops.strp);
	struct bpf_prog *prog = tsk->send_bops.parse_prog;

	return (*prog->bpf_func)(skb, prog->insnsi);
}

static void stap_input_send_strparser(struct strparser *strp,
				      struct sk_buff *skb)
{
	struct stap_sock *tsk = container_of(strp, struct stap_sock,
					       send_bops.strp);
	struct bpf_prog *prog = tsk->send_bops.verdict_prog;

	int rc;

	WARN_ON(tsk->recv_skb);

	/* Run the verdict program to get dispostion of the message */
	rc = (*prog->bpf_func)(skb, prog->insnsi);

	switch (rc) {
	case BPF_OK:
		/* Will push at run queue at end of sendmsg. */
		__skb_queue_tail(&tsk->ready_list, skb);

		break;

	case BPF_DROP:
		kfree_skb(skb);
		return;

	case BPF_REDIRECT:
		/* Not supported yet. */
	case BPF_DISCONNECT:
		/* Kill connection */
	default:
		kfree_skb(skb);
		strp_stop(&tsk->send_bops.strp);
		tsk->sk->sk_err = ECONNABORTED;
		tsk->sk->sk_error_report(tsk->sk);
	}
}

void stap_send_lock(struct strparser *strp)
{
	struct stap_sock *tsk = container_of(strp, struct stap_sock,
					       send_bops.strp);

	lock_sock(tsk->sk);
}

void stap_send_unlock(struct strparser *strp)
{
	struct stap_sock *tsk = container_of(strp, struct stap_sock,
					       send_bops.strp);

	release_sock(tsk->sk);
}

static struct sk_buff *stap_rx_peek(struct stap_sock *tsk)
{
	return tsk->recv_skb;
}

static struct sk_buff *stap_rx_dequeue(struct stap_sock *tsk)
{
	struct sk_buff *skb;

	skb = tsk->recv_skb;
	tsk->recv_skb = NULL;
	strp_unpause(&tsk->recv_bops.strp);

	return skb;
}

static struct sk_buff *stap_wait_data(struct stap_sock *tsk, int flags,
				      long timeo, int *err)
{
	DEFINE_WAIT_FUNC(wait, woken_wake_function);
	struct sock *sk = tsk->sk;
	struct sk_buff *skb;

	while (!(skb = stap_rx_peek(tsk))) {
		if (sk->sk_err) {
			*err = sock_error(sk);
			return NULL;
		}

		if (sock_flag(sk, SOCK_DONE))
			return NULL;

		if ((flags & MSG_DONTWAIT) || !timeo) {
			*err = -EAGAIN;
			return NULL;
		}

		/* Use socket to wait on receive message */
		add_wait_queue(sk_sleep(sk), &wait);
		sk_set_bit(SOCKWQ_ASYNC_WAITDATA, sk);
		sk_wait_event(sk, &timeo, !!stap_rx_peek(tsk), &wait);
		sk_clear_bit(SOCKWQ_ASYNC_WAITDATA, sk);
		remove_wait_queue(sk_sleep(sk), &wait);

		/* Handle signals */
		if (signal_pending(current)) {
			*err = sock_intr_errno(timeo);
			return NULL;
		}
	}

	return skb;
}

static int stap_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
			int flags)
{
	struct stap_sock *tsk = tsk_from_socket(sock);
	struct sock *sk = sock->sk;
	long timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
	struct sk_buff *skb;
	size_t copied = 0;
	size_t slen;
	int err;

	if (unlikely(!tsk))
		return 0;

	lock_sock(sk);

	while (len && (skb = stap_wait_data(tsk, flags, timeo, &err))) {
		struct strp_msg *stm = strp_msg(skb);

		slen = len > stm->full_len ? stm->full_len : len;
		err = skb_copy_datagram_msg(skb, stm->offset, msg, slen);
		if (err < 0)
			goto out_error;

		copied += slen;
		len -= slen;

		if (unlikely(flags & MSG_PEEK)) {
			/* This limits peek to one messge */
			goto out;
		}

		stm->full_len -= slen;
		stm->offset += slen;

		if (!stm->full_len) {
			stap_rx_dequeue(tsk);
			kfree_skb(skb);
			break;
		}
	}

out:
	release_sock(sk);

	return copied;

out_error:
	release_sock(sk);

	return copied ? : err;
}

static ssize_t stap_splice_read(struct socket *sock, loff_t *ppos,
				struct pipe_inode_info *pipe, size_t len,
				unsigned int flags)
{
	struct stap_sock *tsk = tsk_from_socket(sock);
	struct sock *sk = sock->sk;
	long timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);
	struct sk_buff *skb;
	size_t copied = 0;
	size_t slen;
	int err;

	if (unlikely(!tsk))
		return 0;

	lock_sock(sk);

	while (len && (skb = stap_wait_data(tsk, flags, timeo, &err))) {
		struct strp_msg *stm = strp_msg(skb);

		slen = len > stm->full_len ? stm->full_len : len;

		slen = skb_splice_bits(skb, sk, stm->offset, pipe,
				       slen, flags);
		if (slen < 0) {
			err = slen;
			goto out_error;
		}

		stm->full_len -= slen;
		stm->offset += slen;

		copied += slen;
		len -= slen;
		if (!stm->full_len) {
			stap_rx_dequeue(tsk);
			kfree_skb(skb);
			break;
		}
	}

	release_sock(sk);

	return copied;

out_error:
	release_sock(sk);

	return copied ? : err;
}

static int stap_parse_recv_strparser(struct strparser *strp,
				     struct sk_buff *skb)
{
	struct stap_sock *tsk = container_of(strp, struct stap_sock,
					       recv_bops.strp);
	struct bpf_prog *prog = tsk->recv_bops.parse_prog;

	return (*prog->bpf_func)(skb, prog->insnsi);
}

/* Called with lower sock held */
static void stap_input_recv_strparser(struct strparser *strp,
				      struct sk_buff *skb)
{
	struct stap_sock *tsk = container_of(strp, struct stap_sock,
					       recv_bops.strp);
	struct bpf_prog *prog = tsk->recv_bops.verdict_prog;
	int rc;

	WARN_ON(tsk->recv_skb);

	/* Run the verdict program to get dispostion of the message */
	rc = (*prog->bpf_func)(skb, prog->insnsi);

	switch (rc) {
	case BPF_OK:
		tsk->recv_skb = skb;

		strp_pause(&tsk->recv_bops.strp);

		/* Wake up the socket readers */
		tsk->save_data_ready(tsk->sk);

		break;

	case BPF_DROP:
		kfree_skb(skb);
		return;

	case BPF_REDIRECT:
		/* Not supported yet. */
	case BPF_DISCONNECT:
		/* Kill connection */
	default:
		kfree_skb(skb);
		strp_stop(&tsk->recv_bops.strp);
		tsk->sk->sk_err = ECONNABORTED;
		tsk->sk->sk_error_report(tsk->sk);
	}
}

static int stap_load_bpf_prog(int fd, struct bpf_prog **prog)
{
	*prog = bpf_prog_get_type(fd, BPF_PROG_TYPE_SOCKET_FILTER);
	if (IS_ERR(*prog))
		return PTR_ERR(*prog);

	return 0;
}

/* Sock lock must be held */
static int stap_ulp_init(struct sock *sk, char __user *optval, int len)
{
	struct socket *sock = sk->sk_socket;
	struct stap_params zparm;
	struct strp_callbacks cb;
	struct stap_sock *tsk;
	int rc = 0;

	/* Need stap parameters */
	if (len < sizeof(zparm)) {
		rc = -EINVAL;
		goto out_error;
	}

	if (copy_from_user(&zparm, optval, sizeof(zparm))) {
		rc = -EFAULT;
		goto out_error;
	}

	/* Allocate stap socket context */
	tsk = kzalloc(sizeof(*tsk), GFP_KERNEL);
	if (!tsk) {
		rc = -ENOMEM;
		goto out_error;
	}

	rc = stap_load_bpf_prog(zparm.bpf_send_parse_fd,
				&tsk->send_bops.parse_prog);
	if (rc)
		goto send_parse_prog_fail;

	rc = stap_load_bpf_prog(zparm.bpf_send_verdict_fd,
				&tsk->send_bops.verdict_prog);
	if (rc)
		goto send_verdict_prog_fail;

	rc = stap_load_bpf_prog(zparm.bpf_recv_parse_fd,
				&tsk->recv_bops.parse_prog);
	if (rc)
		goto recv_parse_prog_fail;

	rc = stap_load_bpf_prog(zparm.bpf_recv_verdict_fd,
				&tsk->recv_bops.verdict_prog);
	if (rc)
		goto recv_verdict_prog_fail;

	tsk->sk = sk;

	tsk->orig_ops = sock->ops;
	sock->ops = &stap_tcp_stream_ops;

	memset(&cb, 0, sizeof(cb));
	cb.rcv_msg = stap_input_recv_strparser;
	cb.parse_msg = stap_parse_recv_strparser;

	rc = strp_init(&tsk->recv_bops.strp, sk, &cb);
	if (rc)
		goto strp_recv_fail;

	__skb_queue_head_init(&tsk->build_list);
	__skb_queue_head_init(&tsk->ready_list);

	sk->sk_ulp_data = tsk;

	memset(&cb, 0, sizeof(cb));
	cb.rcv_msg = stap_input_send_strparser;
	cb.parse_msg = stap_parse_send_strparser;
	cb.lock = stap_send_lock;
	cb.unlock = stap_send_unlock;

	rc = strp_init(&tsk->send_bops.strp, NULL, &cb);
	if (rc)
		goto strp_send_fail;

	write_lock_bh(&sk->sk_callback_lock);
	tsk->save_data_ready = sk->sk_data_ready;
	tsk->save_write_space = sk->sk_write_space;
	tsk->save_state_change = sk->sk_state_change;
	sk->sk_user_data = tsk;
	sk->sk_data_ready = stap_data_ready;
	sk->sk_write_space = stap_write_space;
	sk->sk_state_change = stap_state_change;
	write_unlock_bh(&sk->sk_callback_lock);

	strp_check_rcv(&tsk->recv_bops.strp);

	return 0;

strp_send_fail:
	strp_stop(&tsk->recv_bops.strp);
	strp_done(&tsk->recv_bops.strp);
strp_recv_fail:
	bpf_prog_put(tsk->recv_bops.verdict_prog);
recv_verdict_prog_fail:
	bpf_prog_put(tsk->recv_bops.parse_prog);
recv_parse_prog_fail:
	bpf_prog_put(tsk->send_bops.verdict_prog);
send_verdict_prog_fail:
	bpf_prog_put(tsk->send_bops.parse_prog);
send_parse_prog_fail:
	kfree(tsk);
out_error:
	return rc;
}

/* Socket lock must not be held here */
static void stap_ulp_release(struct sock *sk)
{
	struct stap_sock *tsk = (struct stap_sock *)sk->sk_ulp_data;

	write_lock_bh(&sk->sk_callback_lock);
	sk->sk_user_data = NULL;
	sk->sk_data_ready = tsk->save_data_ready;
	sk->sk_write_space = tsk->save_write_space;
	sk->sk_state_change = tsk->save_state_change;
	strp_stop(&tsk->recv_bops.strp);
	strp_stop(&tsk->send_bops.strp);
	write_unlock_bh(&sk->sk_callback_lock);

	strp_done(&tsk->recv_bops.strp);
	strp_done(&tsk->send_bops.strp);
	bpf_prog_put(tsk->send_bops.verdict_prog);
	bpf_prog_put(tsk->send_bops.parse_prog);
	bpf_prog_put(tsk->recv_bops.verdict_prog);
	bpf_prog_put(tsk->recv_bops.parse_prog);

	__skb_queue_purge(&tsk->ready_list);
	__skb_queue_purge(&tsk->build_list);

	kfree_skb(tsk->recv_skb);

	/* Release BPF progs */

	sk->sk_ulp_data = NULL;

	kfree(tsk);
}

static struct ulp_ops stap_ulp_ops __read_mostly = {
	.name = "stap",
	.owner = THIS_MODULE,
	.init =  stap_ulp_init,
	.release = stap_ulp_release,
};

static int __init stap_init(void)
{
	int err;

	stap_tcp_stream_ops = inet_stream_ops;
	stap_tcp_stream_ops.sendmsg = stap_sendmsg;
	stap_tcp_stream_ops.sendpage = stap_sendpage;
	stap_tcp_stream_ops.recvmsg = stap_recvmsg;
	stap_tcp_stream_ops.splice_read = stap_splice_read;

	err = ulp_register(&stap_ulp_ops);
	if (err)
		goto register_ulp_fail;

	return 0;

register_ulp_fail:
	return err;
}

static void __exit stap_exit(void)
{
	ulp_unregister(&stap_ulp_ops);
}

module_init(stap_init);
module_exit(stap_exit);
MODULE_AUTHOR("Tom Herbert");
MODULE_LICENSE("GPL");

