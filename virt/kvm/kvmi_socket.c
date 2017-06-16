/*
 * Copyright (C) 2017 Bitdefender S.R.L.
 *
 * The KVMI Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * The KVMI Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the GNU C Library; if not, see
 * <http://www.gnu.org/licenses/>
 */
#include <linux/kernel.h>
#include <linux/net.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/un.h>
#include <linux/namei.h>
#include <linux/kvm_host.h>
#include <linux/kconfig.h>
#include <net/sock.h>
#include <net/net_namespace.h>
#include <net/vsock_addr.h>

#include "kvmi_socket.h"

#define SEND_TIMEOUT_SECS 2

struct worker {
	struct work_struct work;
	wait_queue_head_t wait;	/* accept_cb */
	struct completion finished;
	struct socket *s;
	kvmi_socket_use_cb cb;
	void *cb_ctx;
	void (*orig_sk_state_change)(struct sock *sk);	/* accept_cb */
	void (*orig_sk_data_ready)(struct sock *sk);
	atomic_t knocks;	/* accept_cb */
	bool stopping;
};

static struct workqueue_struct *wq;
static struct kmem_cache *cache;
static struct worker *awork;

static bool should_accept(struct worker *w, struct socket **newsock);
static int __recv(struct socket *s, void *buf, size_t len);
static int __send(struct socket *s, struct kvec *i, size_t n, size_t size);
static int init(int proto, struct sockaddr *addr, size_t addr_len,
		kvmi_socket_use_cb cb, void *cb_ctx);
static int init_socket(int proto, struct sockaddr *addr, size_t addr_len,
		       kvmi_socket_use_cb cb, void *cb_ctx);
static int read_worker_cb(void *_w, void *buf, size_t len);
static int read_socket_cb(void *_s, void *buf, size_t len);
static struct worker *alloc_worker(struct socket *s, kvmi_socket_use_cb cb,
				   void *cb_ctx, work_func_t fct);
static void __socket_close(struct socket *s);
static void accept_cb(struct work_struct *work);
static void data_ready_cb(struct sock *sk);
static void restore_socket_callbacks(struct worker *w);
static void set_socket_callbacks(struct worker *w, bool with_data_ready);
static void state_change_cb(struct sock *sk);
static void stop_cb_on_error(struct worker *w, int err);
static void wakeup_worker(struct worker *w);
static void work_cb(struct work_struct *work);

int kvmi_socket_start_vsock(unsigned int cid, unsigned int port,
			    kvmi_socket_use_cb cb, void *cb_ctx)
{
	struct sockaddr_vm sa;

	vsock_addr_init(&sa, cid, port);

	return init(PF_VSOCK, (struct sockaddr *) &sa, sizeof(sa), cb, cb_ctx);
}

int init(int proto, struct sockaddr *addr, size_t addr_len,
	 kvmi_socket_use_cb cb, void *cb_ctx)
{
	int err;

	wq = alloc_workqueue("kvmi/socket", WQ_CPU_INTENSIVE, 0);
	cache = kmem_cache_create("kvmi/socket", sizeof(struct worker), 0, 0,
				  NULL);

	if (!wq || !cache) {
		kvmi_socket_stop();
		return -ENOMEM;
	}

	err = init_socket(proto, addr, addr_len, cb, cb_ctx);

	if (err) {
		kvm_err("kvmi_socket init: %d\n", err);
		kvmi_socket_stop();
		return err;
	}

	return 0;
}

void kvmi_socket_stop(void)
{
	if (!IS_ERR_OR_NULL(awork)) {
		kvmi_socket_release(awork);
		awork = NULL;
	}

	if (wq) {
		destroy_workqueue(wq);
		wq = NULL;
	}

	kmem_cache_destroy(cache);
	cache = NULL;
}

static void signal_stop(struct worker *w)
{
	WRITE_ONCE(w->stopping, 1);
}

/*
 * !!! MUST NOT be called from use_cb !!!
 */
void kvmi_socket_release(void *_w)
{
	struct worker *w = _w;

	restore_socket_callbacks(w);

	signal_stop(w);
	wakeup_worker(w);

	wait_for_completion(&w->finished);

	if (w->s)
		__socket_close(w->s);

	kmem_cache_free(cache, w);
}

void wakeup_worker(struct worker *w)
{
	if (w == awork)
		wake_up_interruptible(&w->wait);
}

void __socket_close(struct socket *s)
{
	kernel_sock_shutdown(s, SHUT_RDWR);
	sock_release(s);
}

int init_socket(int proto, struct sockaddr *addr, size_t addr_len,
		kvmi_socket_use_cb cb, void *cb_ctx)
{
	struct socket *s;
	int err = sock_create_kern(&init_net, proto, SOCK_STREAM, 0,
				   &s);

	if (err)
		return err;

	err = kernel_bind(s, addr, addr_len);

	if (!err)
		err = kernel_listen(s, 256);

	if (!err) {
		awork = alloc_worker(s, cb, cb_ctx, accept_cb);

		if (IS_ERR(awork)) {
			err = PTR_ERR(awork);
		} else {
			init_waitqueue_head(&awork->wait);
			atomic_set(&awork->knocks, 0);
			set_socket_callbacks(awork, true);
			queue_work(wq, &awork->work);
		}
	}

	if (err)
		sock_release(s);

	return err;
}

struct worker *alloc_worker(struct socket *s, kvmi_socket_use_cb cb,
			    void *cb_ctx, work_func_t fct)
{
	struct worker *w = kmem_cache_zalloc(cache, GFP_KERNEL);

	if (!w)
		return ERR_PTR(-ENOMEM);

	w->s = s;
	w->cb = cb;
	w->cb_ctx = cb_ctx;

	init_completion(&w->finished);
	INIT_WORK(&w->work, fct);

	return w;
}

void set_socket_callbacks(struct worker *w, bool with_data_ready)
{
	struct sock *sk = w->s->sk;

	sk->sk_user_data = w;

	write_lock_bh(&sk->sk_callback_lock);

	if (with_data_ready) {
		w->orig_sk_data_ready = sk->sk_data_ready;
		sk->sk_data_ready = data_ready_cb;
	}

	w->orig_sk_state_change = sk->sk_state_change;
	sk->sk_state_change = state_change_cb;

	write_unlock_bh(&sk->sk_callback_lock);
}

void restore_socket_callbacks(struct worker *w)
{
	struct sock *sk = w->s->sk;

	write_lock_bh(&sk->sk_callback_lock);

	if (w->orig_sk_data_ready)
		sk->sk_data_ready = w->orig_sk_data_ready;

	sk->sk_state_change = w->orig_sk_state_change;

	write_unlock_bh(&sk->sk_callback_lock);
}

void data_ready_cb(struct sock *sk)
{
	struct worker *w = sk->sk_user_data;

	atomic_inc(&w->knocks);
	wakeup_worker(w);
}

void state_change_cb(struct sock *sk)
{
	struct worker *w = sk->sk_user_data;

	signal_stop(w);
	wakeup_worker(w);
}

void accept_cb(struct work_struct *work)
{
	struct worker *w = container_of(work, struct worker, work);

	while (1) {
		struct socket *s = NULL;

		wait_event_interruptible(w->wait, should_accept(w, &s));

		if (READ_ONCE(w->stopping))
			break;

		s->sk->sk_sndtimeo = SEND_TIMEOUT_SECS * HZ;

		if (!w->cb(w->cb_ctx, read_socket_cb, s)) {
			kvm_info("%s(%p) drop the last accepted socket\n",
				 __func__, w);
			__socket_close(s);
		}
	}

	w->cb(w->cb_ctx, NULL, NULL);
	complete_all(&w->finished);
}

bool should_accept(struct worker *w, struct socket **newsock)
{
	if (READ_ONCE(w->stopping))
		return true;

	if (__atomic_add_unless(&w->knocks, -1, 0))
		return (kernel_accept(w->s, newsock, O_NONBLOCK) != -EAGAIN);

	return false;
}

int read_socket_cb(void *s, void *buf, size_t len)
{
	return __recv((struct socket *) s, buf, len);
}

int __recv(struct socket *s, void *buf, size_t len)
{
	struct kvec i = {
		.iov_base = buf,
		.iov_len = len
	};
	struct msghdr m = { };

	int rc = kernel_recvmsg(s, &m, &i, 1, i.iov_len, MSG_WAITALL);

	if (unlikely(rc != len)) {
		struct worker *w = s->sk->sk_user_data;
		int err = (rc >= 0) ? -ETIMEDOUT : rc;

		kvm_info("%s(%p, %u): %d -> %d\n", __func__, w,
			 (unsigned int) len, rc, err);
		return err;
	}

	return 0;
}

void *kvmi_socket_monitor(void *s, kvmi_socket_use_cb cb, void *cb_ctx)
{
	struct worker *w = alloc_worker((struct socket *) s, cb, cb_ctx,
					work_cb);

	if (!IS_ERR(w)) {
		set_socket_callbacks(w, false);
		queue_work(wq, &w->work);
	}

	return w;
}

void work_cb(struct work_struct *work)
{
	struct worker *w = container_of(work, struct worker, work);

	while (w->cb(w->cb_ctx, read_worker_cb, w))
		;

	w->cb(w->cb_ctx, NULL, NULL);
	complete_all(&w->finished);
}

void stop_cb_on_error(struct worker *w, int err)
{
	if (err != -EAGAIN)
		signal_stop(w);
}

int read_worker_cb(void *_w, void *buf, size_t len)
{
	struct worker *w = _w;
	int err;

	if (READ_ONCE(w->stopping))
		return -ENOENT;

	err = __recv(w->s, buf, len);

	if (unlikely(err)) {
		kvm_info("%s(%p): %d\n", __func__, w, err);
		stop_cb_on_error(w, err);
	}

	return err;
}

int kvmi_socket_send(void *_w, struct kvec *i, size_t n, size_t size)
{
	struct worker *w = _w;
	int err;

	if (READ_ONCE(w->stopping))
		return -ENOENT;

	err = __send(w->s, i, n, size);

	if (unlikely(err)) {
		kvm_info("%s(%p): %d\n", __func__, w, err);
		stop_cb_on_error(w, err);
	}

	return err;
}

int __send(struct socket *s, struct kvec *i, size_t n, size_t size)
{
	struct msghdr m = { };
	int rc = kernel_sendmsg(s, &m, i, n, size);

	if (unlikely(rc != size)) {
		int err = (rc > 0) ? -ETIMEDOUT : rc;
		struct worker *w = s->sk->sk_user_data;

		kvm_info("%s(%p): %d -> %d\n", __func__, w, rc, err);
		return err;
	}

	return 0;
}

bool kvmi_socket_is_active(void *_w)
{
	struct worker *w = _w;
	bool running = !completion_done(&w->finished);

	return running;
}
