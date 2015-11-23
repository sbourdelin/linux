/*
 * algif_tls: User-space interface for TLS
 *
 * Copyright (C) 2015, Dave Watson <davejwatson@fb.com>
 *
 * This file provides the user-space API for AEAD ciphers.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <crypto/aead.h>
#include <crypto/if_alg.h>
#include <linux/init.h>
#include <linux/file.h>
#include <linux/list.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/net.h>
#include <net/sock.h>
#include <net/tcp.h>

#define TLS_HEADER_SIZE 13
#define TLS_TAG_SIZE 16
#define TLS_IV_SIZE 8
#define TLS_PADDED_AADLEN 16
#define TLS_MAX_MESSAGE_LEN (1 << 14)

/* Bytes not included in tls msg size field */
#define TLS_FRAMING_SIZE 5

#define TLS_APPLICATION_DATA_MSG 0x17
#define TLS_VERSION 3

struct tls_tfm_pair {
	struct crypto_aead *tfm_send;
	struct crypto_aead *tfm_recv;
	int cur_setkey;
};

static struct workqueue_struct *tls_wq;

struct tls_sg_list {
	unsigned int cur;
	struct scatterlist sg[ALG_MAX_PAGES];
};

#define RSGL_MAX_ENTRIES ALG_MAX_PAGES

struct tls_ctx {
	/* Send and encrypted transmit buffers */
	struct tls_sg_list tsgl;
	struct scatterlist tcsgl[ALG_MAX_PAGES];

	/* Encrypted receive and receive buffers. */
	struct tls_sg_list rcsgl;
	struct af_alg_sgl rsgl[RSGL_MAX_ENTRIES];

	/* Sequence numbers. */
	int iv_set;
	void *iv_send;
	void *iv_recv;

	struct af_alg_completion completion;

	/* Bytes to send */
	unsigned long used;

	/* padded */
	size_t aead_assoclen;
	/* unpadded */
	size_t assoclen;
	struct aead_request aead_req;
	struct aead_request aead_resp;

	bool more;
	bool merge;

	/* Chained TCP socket */
	struct sock *sock;
	struct socket *socket;

	void (*save_data_ready)(struct sock *sk);
	void (*save_write_space)(struct sock *sk);
	void (*save_state_change)(struct sock *sk);
	struct work_struct tx_work;
	struct work_struct rx_work;

	/* This socket for use with above callbacks */
	struct sock *alg_sock;

	/* Send buffer tracking */
	int page_to_send;
	int tcsgl_size;

	/* Recv buffer tracking */
	int recv_wanted;
	int recved_len;

	/* Receive AAD. */
	unsigned char buf[24];
};

static void increment_seqno(u64 *seqno)
{
	u64 seq_h = be64_to_cpu(*seqno);

	seq_h++;
	*seqno = cpu_to_be64(seq_h);
}

static int do_tls_kernel_sendpage(struct sock *sk);

static int tls_wait_for_data(struct sock *sk, unsigned flags)
{
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	long timeout;
	DEFINE_WAIT(wait);
	int err = -ERESTARTSYS;

	if (flags & MSG_DONTWAIT)
		return -EAGAIN;

	set_bit(SOCK_ASYNC_WAITDATA, &sk->sk_socket->flags);

	for (;;) {
		if (signal_pending(current))
			break;
		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
		timeout = MAX_SCHEDULE_TIMEOUT;
		if (sk_wait_event(sk, &timeout,
					ctx->recved_len == ctx->recv_wanted)) {
			err = 0;
			break;
		}
	}
	finish_wait(sk_sleep(sk), &wait);

	clear_bit(SOCK_ASYNC_WAITDATA, &sk->sk_socket->flags);

	return err;
}

static int tls_wait_for_write_space(struct sock *sk, unsigned flags)
{
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	long timeout;
	DEFINE_WAIT(wait);
	int err = -ERESTARTSYS;

	if (flags & MSG_DONTWAIT)
		return -EAGAIN;

	set_bit(SOCK_ASYNC_WAITDATA, &sk->sk_socket->flags);

	for (;;) {
		if (signal_pending(current))
			break;
		prepare_to_wait(sk_sleep(sk), &wait, TASK_INTERRUPTIBLE);
		timeout = MAX_SCHEDULE_TIMEOUT;
		if (sk_wait_event(sk, &timeout, !ctx->page_to_send)) {
			err = 0;
			break;
		}
	}
	finish_wait(sk_sleep(sk), &wait);

	clear_bit(SOCK_ASYNC_WAITDATA, &sk->sk_socket->flags);

	return err;
}

static inline int tls_sndbuf(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;

	return max_t(int, max_t(int, sk->sk_sndbuf & PAGE_MASK, PAGE_SIZE) -
			  ctx->used, 0);
}

static inline bool tls_writable(struct sock *sk)
{
	return tls_sndbuf(sk) >= PAGE_SIZE;
}


static void tls_put_sgl(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	struct tls_sg_list *sgl = &ctx->tsgl;
	struct scatterlist *sg = sgl->sg;
	unsigned int i;

	for (i = 0; i < sgl->cur; i++) {
		if (!sg_page(sg + i))
			continue;

		put_page(sg_page(sg + i));
		sg_assign_page(sg + i, NULL);
	}
	sg_init_table(sg, ALG_MAX_PAGES);
	sgl->cur = 0;
	ctx->used = 0;
	ctx->more = 0;
	ctx->merge = 0;
}

static void tls_wmem_wakeup(struct sock *sk)
{
	struct socket_wq *wq;

	if (!tls_writable(sk))
		return;

	rcu_read_lock();
	wq = rcu_dereference(sk->sk_wq);
	if (wq_has_sleeper(wq))
		wake_up_interruptible_sync_poll(&wq->wait, POLLIN |
							   POLLRDNORM |
							   POLLRDBAND);
	sk_wake_async(sk, SOCK_WAKE_WAITD, POLL_IN);
	rcu_read_unlock();
}

static void tls_put_rcsgl(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	struct tls_sg_list *sgl = &ctx->rcsgl;
	int i;

	for (i = 0; i < sgl->cur; i++)
		put_page(sg_page(&sgl->sg[i]));
	sgl->cur = 0;
	sg_init_table(&sgl->sg[0], ALG_MAX_PAGES);
}


static void tls_sock_state_change(struct sock *sk)
{
	struct tls_ctx *ctx;

	ctx = (struct tls_ctx *)sk->sk_user_data;

	switch (sk->sk_state) {
	case TCP_CLOSE:
	case TCP_CLOSE_WAIT:
	case TCP_ESTABLISHED:
		ctx->alg_sock->sk_state = sk->sk_state;
		ctx->alg_sock->sk_state_change(ctx->alg_sock);
		tls_wmem_wakeup(ctx->alg_sock);

		break;
	default:	/* Everything else is uninteresting */
		break;
	}
}

/* Both socket  lock held */
static ssize_t tls_socket_splice(struct sock *sk,
				struct pipe_inode_info *pipe,
				struct splice_pipe_desc *spd) {
	struct tls_ctx *ctx = (struct tls_ctx *)pipe;
	struct tls_sg_list *sgl = &ctx->rcsgl;

	unsigned int spd_pages = spd->nr_pages;
	int ret = 0;
	int page_nr = 0;

	while (spd->nr_pages > 0) {
		if (sgl->cur < ALG_MAX_PAGES) {
			struct scatterlist *sg = &sgl->sg[sgl->cur];

			sg_assign_page(sg, spd->pages[page_nr]);
			sg->offset = spd->partial[page_nr].offset;
			sg->length = spd->partial[page_nr].len;
			sgl->cur++;

			ret += spd->partial[page_nr].len;
			page_nr++;

			--spd->nr_pages;
		} else {
			sk->sk_err = -ENOMEM;
			break;
		}
	}

	while (page_nr < spd_pages)
		spd->spd_release(spd, page_nr++);

	ctx->recved_len += ret;

	if (ctx->recved_len == ctx->recv_wanted || sk->sk_err)
		tls_wmem_wakeup(ctx->alg_sock);

	return ret;
}

/* Both socket  lock held */
static int tls_tcp_recv(read_descriptor_t *desc, struct sk_buff *skb,
			unsigned int offset, size_t len)
{
	int ret;

	ret = skb_splice_bits(skb, skb->sk, offset, desc->arg.data,
			min(desc->count, len),
			0, tls_socket_splice);
	if (ret > 0)
		desc->count -= ret;

	return ret;
}

static int tls_tcp_read_sock(struct tls_ctx *ctx)
{
	struct sock *sk = ctx->alg_sock;

	struct msghdr msg = {};
	struct kvec iov;
	read_descriptor_t desc;

	desc.arg.data = ctx;
	desc.error = 0;

	lock_sock(sk);

	iov.iov_base = ctx->buf;
	iov.iov_len = TLS_HEADER_SIZE;

	if (ctx->recv_wanted == -1) {
		unsigned int encrypted_size = 0;

		/* Peek at framing.
		 *
		 * We only handle TLS message type 0x17, application_data.
		 *
		 * Otherwise set an error on the socket and let
		 * userspace handle the message types
		 * change_cipher_spec, alert, handshake
		 *
		 */
		int bytes = kernel_recvmsg(ctx->socket, &msg, &iov, 1,
					iov.iov_len, MSG_PEEK | MSG_DONTWAIT);

		if (bytes <= 0)
			goto unlock;

		if (ctx->buf[0] != TLS_APPLICATION_DATA_MSG) {
			sk->sk_err = -EBADMSG;
			desc.error = sk->sk_err;
			goto unlock;
		}

		if (bytes < TLS_HEADER_SIZE)
			goto unlock;


		encrypted_size = ctx->buf[4] | (ctx->buf[3] << 8);

		/* Verify encrypted size looks sane */
		if (encrypted_size > TLS_MAX_MESSAGE_LEN + TLS_TAG_SIZE +
			TLS_HEADER_SIZE - TLS_FRAMING_SIZE) {
			sk->sk_err = -EINVAL;
			desc.error = sk->sk_err;
			goto unlock;
		}
		/* encrypted_size field doesn't include 5 bytes of framing */
		ctx->recv_wanted = encrypted_size + TLS_FRAMING_SIZE;

		/* Flush header bytes.  We peeked at before, we will
		 * handle this message type
		 */
		bytes = kernel_recvmsg(ctx->socket, &msg, &iov, 1,
				iov.iov_len, MSG_DONTWAIT);
		WARN_ON(bytes != TLS_HEADER_SIZE);
		ctx->recved_len = TLS_HEADER_SIZE;
	}

	if (ctx->recv_wanted <= 0)
		goto unlock;

	desc.count = ctx->recv_wanted - ctx->recved_len;

	if (desc.count > 0) {
		lock_sock(ctx->sock);

		tcp_read_sock(ctx->sock, &desc, tls_tcp_recv);

		release_sock(ctx->sock);
	}

unlock:
	if (desc.error)
		tls_wmem_wakeup(ctx->alg_sock);

	release_sock(sk);

	return desc.error;
}

static void tls_tcp_data_ready(struct sock *sk)
{
	struct tls_ctx *ctx;

	read_lock_bh(&sk->sk_callback_lock);

	ctx = (struct tls_ctx *)sk->sk_user_data;

	queue_work(tls_wq, &ctx->rx_work);

	read_unlock_bh(&sk->sk_callback_lock);
}

static void tls_tcp_write_space(struct sock *sk)
{
	struct tls_ctx *ctx;

	read_lock_bh(&sk->sk_callback_lock);

	ctx = (struct tls_ctx *)sk->sk_user_data;

	queue_work(tls_wq, &ctx->tx_work);

	read_unlock_bh(&sk->sk_callback_lock);
}

static void tls_rx_work(struct work_struct *w)
{
	struct tls_ctx *ctx = container_of(w, struct tls_ctx, rx_work);

	tls_tcp_read_sock(ctx);
}

static void tls_tx_work(struct work_struct *w)
{
	struct tls_ctx *ctx = container_of(w, struct tls_ctx, tx_work);
	struct sock *sk = ctx->alg_sock;
	int err;

	lock_sock(sk);

	err = do_tls_kernel_sendpage(sk);
	if (err < 0) {
		/* Hard failure in write, report error on KCM socket */
		pr_warn("TLS: Hard failure on do_tls_sendpage %d\n", err);
		sk->sk_err = -err;
		tls_wmem_wakeup(sk);
		goto out;
	}

out:
	release_sock(sk);
}

static int do_tls_kernel_sendpage(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	int err = 0;
	int i;

	if (ctx->page_to_send == 0)
		return err;
	for (; ctx->page_to_send < ctx->tcsgl_size; ctx->page_to_send++) {
		int flags = MSG_DONTWAIT;

		if (ctx->page_to_send != ctx->tcsgl_size - 1)
			flags |= MSG_MORE;
		err = kernel_sendpage(ctx->sock->sk_socket,
				sg_page(&ctx->tcsgl[ctx->page_to_send]),
				ctx->tcsgl[ctx->page_to_send].offset,
				ctx->tcsgl[ctx->page_to_send].length,
				flags);
		if (err <= 0) {
			if (err == -EAGAIN) {
				/* Don't forward EAGAIN */
				err = 0;
				goto out;
			}
			goto out;
		}
	}

	ctx->page_to_send = 0;

	increment_seqno(ctx->iv_send);


	for (i = 1; i < ctx->tcsgl_size; i++)
		put_page(sg_page(&ctx->tcsgl[i]));

	tls_wmem_wakeup(sk);
out:

	return err;
}

static int do_tls_sendpage(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	struct tls_sg_list *sgl = &ctx->tsgl;

	int used = ctx->used;

	unsigned ivsize =
		crypto_aead_ivsize(crypto_aead_reqtfm(&ctx->aead_req));
	int encrypted_size = ivsize + used +
		crypto_aead_authsize(crypto_aead_reqtfm(&ctx->aead_req));

	/* Ensure enough space in sg list for tag. */
	struct scatterlist *sg = &ctx->tcsgl[1];
	int bytes_needed = used + TLS_HEADER_SIZE + TLS_TAG_SIZE;
	int err = -ENOMEM;

	struct page *p;
	unsigned char *framing;
	unsigned char aad[ctx->aead_assoclen];
	struct scatterlist sgaad[2];

	WARN_ON(used > TLS_MAX_MESSAGE_LEN);

	/* Framing will be put in first sg */
	ctx->tcsgl_size = 1;

	do {
		sg_assign_page(sg, alloc_page(GFP_KERNEL));
		if (!sg_page(sg))
			goto unlock;

		sg_unmark_end(sg);
		sg->offset = 0;
		sg->length = PAGE_SIZE;
		if (bytes_needed < PAGE_SIZE)
			sg->length = bytes_needed;

		ctx->tcsgl_size++;
		sg = &ctx->tcsgl[ctx->tcsgl_size];
		bytes_needed -= PAGE_SIZE;
	} while (bytes_needed > 0);

	p = sg_page(&ctx->tcsgl[1]);

	sg = &ctx->tcsgl[0];

	sg->offset = 0;
	sg->length = TLS_PADDED_AADLEN + TLS_IV_SIZE;
	sg_assign_page(sg, p);

	sg = &ctx->tcsgl[1];
	sg->offset = TLS_HEADER_SIZE;
	sg->length = sg->length - TLS_HEADER_SIZE;

	sg_mark_end(&ctx->tcsgl[ctx->tcsgl_size - 1]);
	framing = page_address(p);

	/* Hardcoded to TLS 1.2 */
	memset(framing, 0, ctx->aead_assoclen);
	framing[0] = TLS_APPLICATION_DATA_MSG;
	framing[1] = TLS_VERSION;
	framing[2] = TLS_VERSION;
	framing[3] = encrypted_size >> 8;
	framing[4] = encrypted_size & 0xff;
	/* Per spec, iv_send can be used as nonce */
	memcpy(framing + 5, ctx->iv_send, TLS_IV_SIZE);

	memset(aad, 0, ctx->aead_assoclen);
	memcpy(aad, ctx->iv_send, TLS_IV_SIZE);

	aad[8] = TLS_APPLICATION_DATA_MSG;
	aad[9] = TLS_VERSION;
	aad[10] = TLS_VERSION;
	aad[11] = used >> 8;
	aad[12] = used & 0xff;

	sg_set_buf(&sgaad[0], aad, ctx->aead_assoclen);
	sg_unmark_end(sgaad);
	sg_chain(sgaad, 2, sgl->sg);

	sg_mark_end(sgl->sg + sgl->cur - 1);
	aead_request_set_crypt(&ctx->aead_req, sgaad, ctx->tcsgl,
			       used, ctx->iv_send);
	aead_request_set_ad(&ctx->aead_req, ctx->assoclen);

	err = af_alg_wait_for_completion(crypto_aead_encrypt(&ctx->aead_req),
					 &ctx->completion);

	if (err) {
		/* EBADMSG implies a valid cipher operation took place */
		if (err == -EBADMSG)
			tls_put_sgl(sk);
		goto unlock;
	}

	ctx->tcsgl[1].length += TLS_HEADER_SIZE;
	ctx->tcsgl[1].offset = 0;

	ctx->page_to_send = 1;

	tls_put_sgl(sk);

	err = do_tls_kernel_sendpage(sk);

unlock:

	return err;
}

static int tls_sendmsg(struct socket *sock, struct msghdr *msg, size_t size)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	struct tls_sg_list *sgl = &ctx->tsgl;
	unsigned ivsize =
		crypto_aead_ivsize(crypto_aead_reqtfm(&ctx->aead_req));
	struct af_alg_control con = {};
	long copied = 0;
	bool init = 0;
	int err = -EINVAL;

	struct socket *csock = NULL;
	struct sock *csk = NULL;

	if (msg->msg_controllen) {
		init = 1;
		err = af_alg_cmsg_send(msg, &con);
		if (err)
			return err;

		if (!ctx->sock) {
			if (!con.op) {
				err = -EINVAL;
				return err;
			}
			csock = sockfd_lookup(con.op, &err);
			if (!csock)
				return -ENOENT;
			csk = csock->sk;
			ctx->sock = csk;
			ctx->socket = csock;
			ctx->alg_sock = sk;
			if (!ctx->sock) {
				err = -EINVAL;
				fput(csock->file);
				return err;
			}
		}

		if (con.iv && con.iv->ivlen != ivsize)
			return -EINVAL;
	}

	lock_sock(sk);

	if (!ctx->more && ctx->used)
		goto unlock;

	if (init) {
		if (con.iv) {
			if (ctx->iv_set == 0) {
				ctx->iv_set = 1;
				memcpy(ctx->iv_send, con.iv->iv, ivsize);
			} else {
				memcpy(ctx->iv_recv, con.iv->iv, ivsize);
			}
		}

		if (con.aead_assoclen) {
			ctx->assoclen = con.aead_assoclen;
			/* Pad out assoclen to 4-byte boundary */
			ctx->aead_assoclen = (con.aead_assoclen + 3) & ~3;
		}

		if (csk) {
			write_lock_bh(&csk->sk_callback_lock);
			ctx->save_data_ready = csk->sk_data_ready;
			ctx->save_write_space = csk->sk_write_space;
			ctx->save_state_change = csk->sk_state_change;
			csk->sk_user_data = ctx;
			csk->sk_data_ready = tls_tcp_data_ready;
			csk->sk_write_space = tls_tcp_write_space;
			csk->sk_state_change = tls_sock_state_change;
			write_unlock_bh(&csk->sk_callback_lock);
		}
	}

	if (sk->sk_err)
		goto out_error;

	while (size) {
		unsigned long len = size;
		struct scatterlist *sg = NULL;

		/* use the existing memory in an allocated page */
		if (ctx->merge) {
			sg = sgl->sg + sgl->cur - 1;
			len = min_t(unsigned long, len,
				    PAGE_SIZE - sg->offset - sg->length);

			if (ctx->page_to_send != 0) {
				err = tls_wait_for_write_space(
					sk, msg->msg_flags);
				if (err)
					goto unlock;
			}

			if (ctx->used + len > TLS_MAX_MESSAGE_LEN) {
				err = do_tls_sendpage(sk);
				if (err < 0)
					goto unlock;

				continue;
			}

			err = memcpy_from_msg(page_address(sg_page(sg)) +
					      sg->offset + sg->length,
					      msg, len);
			if (err)
				goto unlock;

			sg->length += len;
			ctx->merge = (sg->offset + sg->length) &
				     (PAGE_SIZE - 1);

			ctx->used += len;
			copied += len;
			size -= len;
			continue;
		}

		if (!tls_writable(sk)) {
			/* user space sent too much data */
			tls_put_sgl(sk);
			err = -EMSGSIZE;
			goto unlock;
		}

		/* allocate a new page */
		len = min_t(unsigned long, size, tls_sndbuf(sk));
		while (len) {
			int plen = 0;

			if (sgl->cur >= ALG_MAX_PAGES) {
				tls_put_sgl(sk);
				err = -E2BIG;
				goto unlock;
			}

			sg = sgl->sg + sgl->cur;
			plen = min_t(int, len, PAGE_SIZE);

			if (ctx->page_to_send != 0) {
				err = tls_wait_for_write_space(
					sk, msg->msg_flags);
				if (err)
					goto unlock;
			}

			if (ctx->used + plen > TLS_MAX_MESSAGE_LEN) {
				err = do_tls_sendpage(sk);
				if (err < 0)
					goto unlock;
				continue;
			}

			sg_assign_page(sg, alloc_page(GFP_KERNEL));
			err = -ENOMEM;
			if (!sg_page(sg))
				goto unlock;

			err = memcpy_from_msg(page_address(sg_page(sg)),
					      msg, plen);
			if (err) {
				__free_page(sg_page(sg));
				sg_assign_page(sg, NULL);
				goto unlock;
			}

			sg->offset = 0;
			sg->length = plen;
			len -= plen;
			ctx->used += plen;
			copied += plen;
			sgl->cur++;
			size -= plen;
			ctx->merge = plen & (PAGE_SIZE - 1);
		}
	}

	err = 0;

	ctx->more = msg->msg_flags & MSG_MORE;

	if (ctx->more && ctx->used < TLS_MAX_MESSAGE_LEN)
		goto unlock;

	if (ctx->page_to_send != 0) {
		err = tls_wait_for_write_space(sk, msg->msg_flags);
		if (err)
			goto unlock;
	}

	err = do_tls_sendpage(sk);

unlock:
	release_sock(sk);

	return err ?: copied;

out_error:
	err = sk_stream_error(sk, msg->msg_flags, err);
	release_sock(sk);

	return err;
}

static ssize_t tls_sendpage(struct socket *sock, struct page *page,
			int offset, size_t size, int flags)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	struct tls_sg_list *sgl = &ctx->tsgl;
	int err = -EINVAL;

	if (flags & MSG_SENDPAGE_NOTLAST)
		flags |= MSG_MORE;

	if (sgl->cur >= ALG_MAX_PAGES)
		return -E2BIG;

	lock_sock(sk);

	if (sk->sk_err)
		goto out_error;

	if (ctx->page_to_send != 0) {
		err = tls_wait_for_write_space(sk, flags);
		if (err)
			goto unlock;
	}

	if (size + ctx->used > TLS_MAX_MESSAGE_LEN) {
		err = do_tls_sendpage(sk);
		if (err < 0)
			goto unlock;
		err = -EINVAL;
	}

	if (!ctx->more && ctx->used)
		goto unlock;

	if (!size)
		goto done;

	if (!tls_writable(sk)) {
		/* user space sent too much data */
		tls_put_sgl(sk);
		err = -EMSGSIZE;
		goto unlock;
	}

	ctx->merge = 0;

	get_page(page);
	sg_set_page(sgl->sg + sgl->cur, page, size, offset);
	sgl->cur++;
	ctx->used += size;

	err = 0;

done:
	ctx->more = flags & MSG_MORE;

	if (ctx->more && ctx->used < TLS_MAX_MESSAGE_LEN)
		goto unlock;

	err = do_tls_sendpage(sk);

unlock:
	release_sock(sk);

	return err < 0 ? err : size;

out_error:
	err = sk_stream_error(sk, flags, err);
	release_sock(sk);

	return err;
}

static int tls_recvmsg(struct socket *sock, struct msghdr *msg,
		size_t ignored, int flags)
{
	struct sock *sk = sock->sk;
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	unsigned int i = 0;
	int err = -EINVAL;
	size_t outlen = 0;
	size_t usedpages = 0;
	unsigned int cnt = 0;

	char aad_unneeded[ctx->aead_assoclen];
	struct scatterlist outaad[2];

	struct tls_sg_list *sgl = &ctx->rcsgl;
	struct scatterlist aadsg[2];

	char buf[11];
	int used;
	char *aad;

	char nonce[TLS_IV_SIZE];

	/* Limit number of IOV blocks to be accessed below */
	if (msg->msg_iter.nr_segs > RSGL_MAX_ENTRIES)
		return -ENOMSG;

	tls_tcp_read_sock(ctx);

	lock_sock(sk);

	if (sk->sk_err)
		goto out_error;

	if (ctx->recved_len != ctx->recv_wanted) {
		err = tls_wait_for_data(sk, flags);
		if (err)
			goto unlock;
	}

	sg_set_buf(outaad, aad_unneeded, ctx->aead_assoclen);
	sg_unmark_end(outaad);
	sg_chain(outaad, 2, &ctx->rsgl[0].sg[0]);

	outlen = ctx->recv_wanted - TLS_FRAMING_SIZE - ctx->aead_assoclen;

	/* convert iovecs of output buffers into scatterlists */
	while (iov_iter_count(&msg->msg_iter)) {
		size_t seglen = min_t(size_t, iov_iter_count(&msg->msg_iter),
				      (outlen - usedpages));

		/* make one iovec available as scatterlist */
		err = af_alg_make_sg(&ctx->rsgl[cnt], &msg->msg_iter,
				     seglen);
		if (err < 0)
			goto unlock;
		usedpages += err;
		/* chain the new scatterlist with previous one */
		if (cnt)
			af_alg_link_sg(&ctx->rsgl[cnt-1], &ctx->rsgl[cnt]);

		/* we do not need more iovecs as we have sufficient memory */
		if (outlen <= usedpages)
			break;
		iov_iter_advance(&msg->msg_iter, err);
		cnt++;
	}

	err = -EINVAL;

	/* ensure output buffer is sufficiently large */
	if (usedpages < outlen)
		goto unlock;

	memset(buf, 0, sizeof(buf));

	used = ctx->recv_wanted - ctx->aead_assoclen - TLS_FRAMING_SIZE;

	aad = ctx->buf;

	sg_set_buf(aadsg, ctx->buf, ctx->aead_assoclen);
	sg_unmark_end(aadsg);
	sg_chain(aadsg, 2, sgl->sg);

	memcpy(nonce, aad + TLS_FRAMING_SIZE, TLS_IV_SIZE);
	memcpy(aad, ctx->iv_recv, TLS_IV_SIZE);

	aad[8] = TLS_APPLICATION_DATA_MSG;
	aad[9] = TLS_VERSION;
	aad[10] = TLS_VERSION;
	aad[11] = used >> 8;
	aad[12] = used & 0xff;

	sg_mark_end(sgl->sg + sgl->cur - 1);
	aead_request_set_crypt(&ctx->aead_resp, aadsg, outaad,
			ctx->recv_wanted + TLS_TAG_SIZE
			- TLS_FRAMING_SIZE - ctx->aead_assoclen,
			nonce);
	aead_request_set_ad(&ctx->aead_resp, ctx->assoclen);

	err = af_alg_wait_for_completion(crypto_aead_decrypt(&ctx->aead_resp),
	&ctx->completion);

	if (err) {
		/* EBADMSG implies a valid cipher operation took place */
		goto unlock;
	} else {
		ctx->recv_wanted = -1;
		ctx->recved_len = 0;
	}

	increment_seqno(ctx->iv_recv);

	err = 0;

unlock:
	tls_put_rcsgl(sk);

	for (i = 0; i < cnt; i++)
		af_alg_free_sg(&ctx->rsgl[i]);

	queue_work(tls_wq, &ctx->rx_work);
	release_sock(sk);

	return err ? err : outlen;

out_error:
	err = sk_stream_error(sk, msg->msg_flags, err);
	release_sock(sk);

	return err;
}


static struct proto_ops algif_tls_ops = {
	.family		=	PF_ALG,

	.connect	=	sock_no_connect,
	.socketpair	=	sock_no_socketpair,
	.getname	=	sock_no_getname,
	.ioctl		=	sock_no_ioctl,
	.listen		=	sock_no_listen,
	.shutdown	=	sock_no_shutdown,
	.getsockopt	=	sock_no_getsockopt,
	.mmap		=	sock_no_mmap,
	.bind		=	sock_no_bind,
	.accept		=	sock_no_accept,
	.setsockopt	=	sock_no_setsockopt,

	.release	=	af_alg_release,
	.sendmsg	=	tls_sendmsg,
	.sendpage	=	tls_sendpage,
	.recvmsg	=	tls_recvmsg,
	.poll		=	sock_no_poll,
};

static void *tls_bind(const char *name, u32 type, u32 mask)
{
	struct tls_tfm_pair *pair = kmalloc(sizeof(struct tls_tfm_pair),
					GFP_KERNEL);

	if (!pair)
		return NULL;
	pair->tfm_send = crypto_alloc_aead(name, type, mask);
	if (!pair->tfm_send)
		goto error;
	pair->tfm_recv = crypto_alloc_aead(name, type, mask);
	if (!pair->tfm_recv)
		goto error;

	pair->cur_setkey = 0;

	return pair;

error:
	if (pair->tfm_send)
		crypto_free_aead(pair->tfm_send);
	if (pair->tfm_recv)
		crypto_free_aead(pair->tfm_recv);
	kfree(pair);

	return NULL;
}

static void tls_release(void *private)
{
	struct tls_tfm_pair *pair = private;

	if (pair) {
		if (pair->tfm_send)
			crypto_free_aead(pair->tfm_send);
		if (pair->tfm_recv)
			crypto_free_aead(pair->tfm_recv);
		kfree(private);
	}
}

static int tls_setauthsize(void *private, unsigned int authsize)
{
	struct tls_tfm_pair *pair = private;

	crypto_aead_setauthsize(pair->tfm_recv, authsize);
	return crypto_aead_setauthsize(pair->tfm_send, authsize);
}

static int tls_setkey(void *private, const u8 *key, unsigned int keylen)
{
	struct tls_tfm_pair *pair = private;

	if (pair->cur_setkey == 0) {
		pair->cur_setkey = 1;
		return crypto_aead_setkey(pair->tfm_send, key, keylen);
	} else {
		return crypto_aead_setkey(pair->tfm_recv, key, keylen);
	}
}

static void tls_sock_destruct(struct sock *sk)
{
	struct alg_sock *ask = alg_sk(sk);
	struct tls_ctx *ctx = ask->private;
	unsigned int ivlen = crypto_aead_ivsize(
				crypto_aead_reqtfm(&ctx->aead_req));



	cancel_work_sync(&ctx->tx_work);
	cancel_work_sync(&ctx->rx_work);

	/* Stop getting callbacks from TCP socket. */
	write_lock_bh(&ctx->sock->sk_callback_lock);
	if (ctx->sock->sk_user_data) {
		ctx->sock->sk_user_data = NULL;
		ctx->sock->sk_data_ready = ctx->save_data_ready;
		ctx->sock->sk_write_space = ctx->save_write_space;
		ctx->sock->sk_state_change = ctx->save_state_change;
	}
	write_unlock_bh(&ctx->sock->sk_callback_lock);

	tls_put_sgl(sk);
	sock_kzfree_s(sk, ctx->iv_send, ivlen);
	sock_kzfree_s(sk, ctx->iv_recv, ivlen);
	sock_kfree_s(sk, ctx, sizeof(*ctx));
	af_alg_release_parent(sk);
}

static int tls_accept_parent(void *private, struct sock *sk)
{
	struct tls_ctx *ctx;
	struct alg_sock *ask = alg_sk(sk);
	struct tls_tfm_pair *pair = private;

	unsigned int len = sizeof(*ctx);
	unsigned int ivlen = crypto_aead_ivsize(pair->tfm_send);

	ctx = sock_kmalloc(sk, len, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
	memset(ctx, 0, len);

	ctx->iv_send = sock_kmalloc(sk, ivlen, GFP_KERNEL);
	if (!ctx->iv_send) {
		sock_kfree_s(sk, ctx, len);
		return -ENOMEM;
	}
	memset(ctx->iv_send, 0, ivlen);

	ctx->iv_recv = sock_kmalloc(sk, ivlen, GFP_KERNEL);
	if (!ctx->iv_recv) {
		sock_kfree_s(sk, ctx, len);
		return -ENOMEM;
	}
	memset(ctx->iv_recv, 0, ivlen);

	ctx->aead_assoclen = 0;
	ctx->recv_wanted = -1;
	af_alg_init_completion(&ctx->completion);
	INIT_WORK(&ctx->tx_work, tls_tx_work);
	INIT_WORK(&ctx->rx_work, tls_rx_work);

	ask->private = ctx;

	aead_request_set_tfm(&ctx->aead_req, pair->tfm_send);
	aead_request_set_tfm(&ctx->aead_resp, pair->tfm_recv);
	aead_request_set_callback(&ctx->aead_req, CRYPTO_TFM_REQ_MAY_BACKLOG,
				  af_alg_complete, &ctx->completion);

	sk->sk_destruct = tls_sock_destruct;

	return 0;
}

static const struct af_alg_type algif_type_tls = {
	.bind		=	tls_bind,
	.release	=	tls_release,
	.setkey		=	tls_setkey,
	.setauthsize	=	tls_setauthsize,
	.accept		=	tls_accept_parent,
	.ops		=	&algif_tls_ops,
	.name		=	"tls",
	.owner		=	THIS_MODULE
};

static int __init algif_tls_init(void)
{
	int err = -ENOMEM;

	tls_wq = create_singlethread_workqueue("ktlsd");
	if (!tls_wq)
		goto error;

	err = af_alg_register_type(&algif_type_tls);

	if (!err)
		return 0;
error:
	if (tls_wq)
		destroy_workqueue(tls_wq);
	return err;
}

static void __exit algif_tls_exit(void)
{
	af_alg_unregister_type(&algif_type_tls);
	destroy_workqueue(tls_wq);
}

module_init(algif_tls_init);
module_exit(algif_tls_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dave Watson <davejwatson@fb.com>");
MODULE_DESCRIPTION("TLS kernel crypto API net interface");
