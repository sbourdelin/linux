/*
 * Copyright (c) 2016-2017, Mellanox Technologies. All rights reserved.
 * Copyright (c) 2016-2017, Dave Watson <davejwatson@fb.com>. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/module.h>

#include <net/tcp.h>
#include <net/inet_common.h>
#include <linux/highmem.h>
#include <linux/netdevice.h>
#include <linux/sched/signal.h>
#include <linux/inetdevice.h>

#include <net/tls.h>

MODULE_AUTHOR("Mellanox Technologies");
MODULE_DESCRIPTION("Transport Layer Security Support");
MODULE_LICENSE("Dual BSD/GPL");

enum {
	TLS_BASE_TX,
	TLS_SW_TX,
	TLS_FULL_HW, /* TLS record processed Inline */
	TLS_NUM_CONFIG,
};

static LIST_HEAD(device_list);
static DEFINE_MUTEX(device_mutex);
static struct proto tls_prots[TLS_NUM_CONFIG];

static inline void update_sk_prot(struct sock *sk, struct tls_context *ctx)
{
	sk->sk_prot = &tls_prots[ctx->tx_conf];
}

int wait_on_pending_writer(struct sock *sk, long *timeo)
{
	int rc = 0;
	DEFINE_WAIT_FUNC(wait, woken_wake_function);

	add_wait_queue(sk_sleep(sk), &wait);
	while (1) {
		if (!*timeo) {
			rc = -EAGAIN;
			break;
		}

		if (signal_pending(current)) {
			rc = sock_intr_errno(*timeo);
			break;
		}

		if (sk_wait_event(sk, timeo, !sk->sk_write_pending, &wait))
			break;
	}
	remove_wait_queue(sk_sleep(sk), &wait);
	return rc;
}

int tls_push_sg(struct sock *sk,
		struct tls_context *ctx,
		struct scatterlist *sg,
		u16 first_offset,
		int flags)
{
	int sendpage_flags = flags | MSG_SENDPAGE_NOTLAST;
	int ret = 0;
	struct page *p;
	size_t size;
	int offset = first_offset;

	size = sg->length - offset;
	offset += sg->offset;

	while (1) {
		if (sg_is_last(sg))
			sendpage_flags = flags;

		/* is sending application-limited? */
		tcp_rate_check_app_limited(sk);
		p = sg_page(sg);
retry:
		ret = do_tcp_sendpages(sk, p, offset, size, sendpage_flags);

		if (ret != size) {
			if (ret > 0) {
				offset += ret;
				size -= ret;
				goto retry;
			}

			offset -= sg->offset;
			ctx->partially_sent_offset = offset;
			ctx->partially_sent_record = (void *)sg;
			return ret;
		}

		put_page(p);
		sk_mem_uncharge(sk, sg->length);
		sg = sg_next(sg);
		if (!sg)
			break;

		offset = sg->offset;
		size = sg->length;
	}

	clear_bit(TLS_PENDING_CLOSED_RECORD, &ctx->flags);

	return 0;
}

static int tls_handle_open_record(struct sock *sk, int flags)
{
	struct tls_context *ctx = tls_get_ctx(sk);

	if (tls_is_pending_open_record(ctx))
		return ctx->push_pending_record(sk, flags);

	return 0;
}

int tls_proccess_cmsg(struct sock *sk, struct msghdr *msg,
		      unsigned char *record_type)
{
	struct cmsghdr *cmsg;
	int rc = -EINVAL;

	for_each_cmsghdr(cmsg, msg) {
		if (!CMSG_OK(msg, cmsg))
			return -EINVAL;
		if (cmsg->cmsg_level != SOL_TLS)
			continue;

		switch (cmsg->cmsg_type) {
		case TLS_SET_RECORD_TYPE:
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(*record_type)))
				return -EINVAL;

			if (msg->msg_flags & MSG_MORE)
				return -EINVAL;

			rc = tls_handle_open_record(sk, msg->msg_flags);
			if (rc)
				return rc;

			*record_type = *(unsigned char *)CMSG_DATA(cmsg);
			rc = 0;
			break;
		default:
			return -EINVAL;
		}
	}

	return rc;
}

int tls_push_pending_closed_record(struct sock *sk, struct tls_context *ctx,
				   int flags, long *timeo)
{
	struct scatterlist *sg;
	u16 offset;

	if (!tls_is_partially_sent_record(ctx))
		return ctx->push_pending_record(sk, flags);

	sg = ctx->partially_sent_record;
	offset = ctx->partially_sent_offset;

	ctx->partially_sent_record = NULL;
	return tls_push_sg(sk, ctx, sg, offset, flags);
}

static void tls_write_space(struct sock *sk)
{
	struct tls_context *ctx = tls_get_ctx(sk);

	if (!sk->sk_write_pending && tls_is_pending_closed_record(ctx)) {
		gfp_t sk_allocation = sk->sk_allocation;
		int rc;
		long timeo = 0;

		sk->sk_allocation = GFP_ATOMIC;
		rc = tls_push_pending_closed_record(sk, ctx,
						    MSG_DONTWAIT |
						    MSG_NOSIGNAL,
						    &timeo);
		sk->sk_allocation = sk_allocation;

		if (rc < 0)
			return;
	}

	ctx->sk_write_space(sk);
}

static void tls_sk_proto_close(struct sock *sk, long timeout)
{
	struct tls_context *ctx = tls_get_ctx(sk);
	long timeo = sock_sndtimeo(sk, 0);
	void (*sk_proto_close)(struct sock *sk, long timeout);

	lock_sock(sk);
	sk_proto_close = ctx->sk_proto_close;

	if (ctx->tx_conf == TLS_BASE_TX) {
		kfree(ctx);
		goto skip_tx_cleanup;
	}

	if (!tls_complete_pending_work(sk, ctx, 0, &timeo))
		tls_handle_open_record(sk, 0);

	if (ctx->partially_sent_record) {
		struct scatterlist *sg = ctx->partially_sent_record;

		while (1) {
			put_page(sg_page(sg));
			sk_mem_uncharge(sk, sg->length);

			if (sg_is_last(sg))
				break;
			sg++;
		}
	}

	kfree(ctx->rec_seq);
	kfree(ctx->iv);

	if (ctx->tx_conf == TLS_SW_TX)
		tls_sw_free_tx_resources(sk);

skip_tx_cleanup:
	release_sock(sk);
	sk_proto_close(sk, timeout);
}

static int do_tls_getsockopt_tx(struct sock *sk, char __user *optval,
				int __user *optlen)
{
	int rc = 0;
	struct tls_context *ctx = tls_get_ctx(sk);
	struct tls_crypto_info *crypto_info;
	int len;

	if (get_user(len, optlen))
		return -EFAULT;

	if (!optval || (len < sizeof(*crypto_info))) {
		rc = -EINVAL;
		goto out;
	}

	if (!ctx) {
		rc = -EBUSY;
		goto out;
	}

	/* get user crypto info */
	crypto_info = &ctx->crypto_send;

	if (!TLS_CRYPTO_INFO_READY(crypto_info)) {
		rc = -EBUSY;
		goto out;
	}

	if (len == sizeof(*crypto_info)) {
		if (copy_to_user(optval, crypto_info, sizeof(*crypto_info)))
			rc = -EFAULT;
		goto out;
	}

	switch (crypto_info->cipher_type) {
	case TLS_CIPHER_AES_GCM_128: {
		struct tls12_crypto_info_aes_gcm_128 *
		  crypto_info_aes_gcm_128 =
		  container_of(crypto_info,
			       struct tls12_crypto_info_aes_gcm_128,
			       info);

		if (len != sizeof(*crypto_info_aes_gcm_128)) {
			rc = -EINVAL;
			goto out;
		}
		lock_sock(sk);
		memcpy(crypto_info_aes_gcm_128->iv, ctx->iv,
		       TLS_CIPHER_AES_GCM_128_IV_SIZE);
		release_sock(sk);
		if (copy_to_user(optval,
				 crypto_info_aes_gcm_128,
				 sizeof(*crypto_info_aes_gcm_128)))
			rc = -EFAULT;
		break;
	}
	default:
		rc = -EINVAL;
	}

out:
	return rc;
}

static int do_tls_getsockopt(struct sock *sk, int optname,
			     char __user *optval, int __user *optlen)
{
	int rc = 0;

	switch (optname) {
	case TLS_TX:
		rc = do_tls_getsockopt_tx(sk, optval, optlen);
		break;
	default:
		rc = -ENOPROTOOPT;
		break;
	}
	return rc;
}

static int tls_getsockopt(struct sock *sk, int level, int optname,
			  char __user *optval, int __user *optlen)
{
	struct tls_context *ctx = tls_get_ctx(sk);

	if (level != SOL_TLS)
		return ctx->getsockopt(sk, level, optname, optval, optlen);

	return do_tls_getsockopt(sk, optname, optval, optlen);
}

static int do_tls_setsockopt_tx(struct sock *sk, char __user *optval,
				unsigned int optlen)
{
	struct tls_crypto_info *crypto_info;
	struct tls_context *ctx = tls_get_ctx(sk);
	int rc = 0;
	int tx_conf;

	if (!optval || (optlen < sizeof(*crypto_info))) {
		rc = -EINVAL;
		goto out;
	}

	crypto_info = &ctx->crypto_send;
	/* Currently we don't support set crypto info more than one time */
	if (TLS_CRYPTO_INFO_READY(crypto_info))
		goto out;

	rc = copy_from_user(crypto_info, optval, sizeof(*crypto_info));
	if (rc) {
		rc = -EFAULT;
		goto out;
	}

	/* check version */
	if (crypto_info->version != TLS_1_2_VERSION) {
		rc = -ENOTSUPP;
		goto err_crypto_info;
	}

	switch (crypto_info->cipher_type) {
	case TLS_CIPHER_AES_GCM_128: {
		if (optlen != sizeof(struct tls12_crypto_info_aes_gcm_128)) {
			rc = -EINVAL;
			goto out;
		}
		rc = copy_from_user(crypto_info + 1, optval + sizeof(*crypto_info),
				    optlen - sizeof(*crypto_info));
		if (rc) {
			rc = -EFAULT;
			goto err_crypto_info;
		}
		break;
	}
	default:
		rc = -EINVAL;
		goto out;
	}

	/* currently SW is default, we will have ethtool in future */
	rc = tls_set_sw_offload(sk, ctx);
	tx_conf = TLS_SW_TX;
	if (rc)
		goto err_crypto_info;

	ctx->tx_conf = tx_conf;
	update_sk_prot(sk, ctx);
	ctx->sk_write_space = sk->sk_write_space;
	sk->sk_write_space = tls_write_space;
	goto out;

err_crypto_info:
	memset(crypto_info, 0, sizeof(*crypto_info));
out:
	return rc;
}

static int do_tls_setsockopt(struct sock *sk, int optname,
			     char __user *optval, unsigned int optlen)
{
	int rc = 0;

	switch (optname) {
	case TLS_TX:
		lock_sock(sk);
		rc = do_tls_setsockopt_tx(sk, optval, optlen);
		release_sock(sk);
		break;
	default:
		rc = -ENOPROTOOPT;
		break;
	}
	return rc;
}

static int tls_setsockopt(struct sock *sk, int level, int optname,
			  char __user *optval, unsigned int optlen)
{
	struct tls_context *ctx = tls_get_ctx(sk);

	if (level != SOL_TLS)
		return ctx->setsockopt(sk, level, optname, optval, optlen);

	return do_tls_setsockopt(sk, optname, optval, optlen);
}

static struct net_device *find_netdev(struct sock *sk)
{
	struct net_device *netdev = NULL;

	netdev = __ip_dev_find(&init_net, inet_sk(sk)->inet_rcv_saddr, false);
	return netdev;
}

static int get_tls_prot(struct sock *sk)
{
	struct tls_context *ctx = tls_get_ctx(sk);
	struct net_device *netdev;
	struct tls_device *dev;

	/* Device bound to specific IP */
	if (inet_sk(sk)->inet_rcv_saddr) {
		netdev = find_netdev(sk);
		if (!netdev)
			goto out;

		/* Device supports Inline record processing */
		if (!(netdev->features & NETIF_F_HW_TLS_INLINE))
			goto out;

		mutex_lock(&device_mutex);
		list_for_each_entry(dev, &device_list, dev_list) {
			if (dev->netdev && dev->netdev(dev, netdev))
				break;
		}
		mutex_unlock(&device_mutex);

		ctx->tx_conf = TLS_FULL_HW;
		if (dev->prot)
			dev->prot(dev, sk);
	} else { /* src address not known or INADDR_ANY */
		mutex_lock(&device_mutex);
		list_for_each_entry(dev, &device_list, dev_list) {
			if (dev->feature && dev->feature(dev)) {
				ctx->tx_conf = TLS_FULL_HW;
				break;
			}
		}
		mutex_unlock(&device_mutex);
		update_sk_prot(sk, ctx);
	}
out:
	return ctx->tx_conf;
}

static int tls_hw_prot(struct sock *sk)
{
	/* search registered tls device for netdev */
	return get_tls_prot(sk);
}

static void tls_hw_unhash(struct sock *sk)
{
	struct tls_device *dev;

	mutex_lock(&device_mutex);
	list_for_each_entry(dev, &device_list, dev_list) {
		if (dev->unhash)
			dev->unhash(dev, sk);
	}
	mutex_unlock(&device_mutex);
	tcp_prot.unhash(sk);
}

static int tls_hw_hash(struct sock *sk)
{
	struct tls_device *dev;
	int err;

	err = tcp_prot.hash(sk);
	mutex_lock(&device_mutex);
	list_for_each_entry(dev, &device_list, dev_list) {
		if (dev->hash)
			err |= dev->hash(dev, sk);
	}
	mutex_unlock(&device_mutex);

	if (err)
		tls_hw_unhash(sk);
	return err;
}

static int tls_init(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tls_context *ctx;
	int rc = 0;

	/* allocate tls context */
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rc = -ENOMEM;
		goto out;
	}
	icsk->icsk_ulp_data = ctx;
	ctx->setsockopt = sk->sk_prot->setsockopt;
	ctx->getsockopt = sk->sk_prot->getsockopt;
	ctx->sk_proto_close = sk->sk_prot->close;

	ctx->tx_conf = TLS_BASE_TX;
	if (tls_hw_prot(sk) == TLS_FULL_HW)
		goto out;

	update_sk_prot(sk, ctx);
out:
	return rc;
}

static struct tcp_ulp_ops tcp_tls_ulp_ops __read_mostly = {
	.name			= "tls",
	.owner			= THIS_MODULE,
	.init			= tls_init,
};

static void build_protos(struct proto *prot, struct proto *base)
{
	prot[TLS_BASE_TX] = *base;
	prot[TLS_BASE_TX].setsockopt	= tls_setsockopt;
	prot[TLS_BASE_TX].getsockopt	= tls_getsockopt;
	prot[TLS_BASE_TX].close		= tls_sk_proto_close;

	prot[TLS_SW_TX] = prot[TLS_BASE_TX];
	prot[TLS_SW_TX].sendmsg		= tls_sw_sendmsg;
	prot[TLS_SW_TX].sendpage	= tls_sw_sendpage;

	prot[TLS_FULL_HW]               = prot[TLS_BASE_TX];
	prot[TLS_FULL_HW].hash          = tls_hw_hash;
	prot[TLS_FULL_HW].unhash        = tls_hw_unhash;
}

void tls_register_device(struct tls_device *device)
{
	mutex_lock(&device_mutex);
	list_add_tail(&device->dev_list, &device_list);
	mutex_unlock(&device_mutex);
}
EXPORT_SYMBOL(tls_register_device);

void tls_unregister_device(struct tls_device *device)
{
	mutex_lock(&device_mutex);
	list_del(&device->dev_list);
	mutex_unlock(&device_mutex);
}
EXPORT_SYMBOL(tls_unregister_device);

static int __init tls_register(void)
{
	build_protos(tls_prots, &tcp_prot);

	tcp_register_ulp(&tcp_tls_ulp_ops);

	return 0;
}

static void __exit tls_unregister(void)
{
	tcp_unregister_ulp(&tcp_tls_ulp_ops);
}

module_init(tls_register);
module_exit(tls_unregister);
