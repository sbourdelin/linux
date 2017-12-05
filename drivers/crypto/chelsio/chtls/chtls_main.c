/*
 * Copyright (c) 2017 Chelsio Communications, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Written by: Atul Gupta (atul.gupta@chelsio.com)
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/socket.h>
#include <linux/hash.h>
#include <linux/in.h>
#include <linux/net.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <net/tls.h>

#include "chtls.h"
#include "chtls_cm.h"

#define DRV_NAME "chtls"

/*
 * chtls device management
 * maintains a list of the chtls devices
 */
static LIST_HEAD(cdev_list);
static DEFINE_MUTEX(cdev_mutex);
static DEFINE_MUTEX(cdev_list_lock);

static struct proto chtls_base_prot;
static struct proto chtls_cpl_prot;
static DEFINE_MUTEX(notify_mutex);
static RAW_NOTIFIER_HEAD(listen_notify_list);
struct request_sock_ops chtls_rsk_ops;
static uint send_page_order = (14 - PAGE_SHIFT < 0) ? 0 : 14 - PAGE_SHIFT;

int register_listen_notifier(struct notifier_block *nb)
{
	int err;

	mutex_lock(&notify_mutex);
	err = raw_notifier_chain_register(&listen_notify_list, nb);
	mutex_unlock(&notify_mutex);
	return err;
}

int unregister_listen_notifier(struct notifier_block *nb)
{
	int err;

	mutex_lock(&notify_mutex);
	err = raw_notifier_chain_unregister(&listen_notify_list, nb);
	mutex_unlock(&notify_mutex);
	return err;
}

int listen_notify_handler(struct notifier_block *this,
			  unsigned long event, void *data)
{
	struct sock *sk = data;
	struct chtls_dev *cdev;
	int ret =  NOTIFY_DONE;

	switch (event) {
	case CHTLS_LISTEN_START:
	case CHTLS_LISTEN_STOP:
		mutex_lock(&cdev_list_lock);
		list_for_each_entry(cdev, &cdev_list, list) {
			if (event == CHTLS_LISTEN_START)
				ret = chtls_listen_start(cdev, sk);
			else
				chtls_listen_stop(cdev, sk);
		}
		mutex_unlock(&cdev_list_lock);
		break;
	}
	return ret;
}

static struct notifier_block listen_notifier = {
	.notifier_call = listen_notify_handler
};

static int listen_backlog_rcv(struct sock *sk, struct sk_buff *skb)
{
	if (likely(skb_transport_header(skb) != skb_network_header(skb)))
		return tcp_v4_do_rcv(sk, skb);
	BLOG_SKB_CB(skb)->backlog_rcv(sk, skb);
	return 0;
}

static int chtls_start_listen(struct sock *sk)
{
	int err;

	if (sk->sk_protocol != IPPROTO_TCP)
		return -EPROTONOSUPPORT;

	if (sk->sk_family == PF_INET &&
	    LOOPBACK(inet_sk(sk)->inet_rcv_saddr))
		return -EADDRNOTAVAIL;

	sk->sk_backlog_rcv = listen_backlog_rcv;
	mutex_lock(&notify_mutex);
	err = raw_notifier_call_chain(&listen_notify_list, 0, sk);
	mutex_unlock(&notify_mutex);
	return err;
}

static int chtls_hash(struct sock *sk)
{
	int err;

	err = tcp_prot.hash(sk);
	if (sk->sk_state == TCP_LISTEN)
		err |= chtls_start_listen(sk);

	if (err)
		tcp_prot.unhash(sk);
	return err;
}

int chtls_stop_listen(struct sock *sk)
{
	if (sk->sk_protocol != IPPROTO_TCP)
		return -EPROTONOSUPPORT;

	mutex_lock(&notify_mutex);
	raw_notifier_call_chain(&listen_notify_list, 1, sk);
	mutex_unlock(&notify_mutex);
	return 0;
}

static void chtls_unhash(struct sock *sk)
{
	if (sk->sk_state == TCP_LISTEN)
		chtls_stop_listen(sk);
	tcp_prot.unhash(sk);
}

static void chtls_lsk_close(struct sock *sk, long timeout)
{
	struct tls_context *ctx = tls_get_ctx(sk);
	void (*sk_proto_close)(struct sock *sk, long timeout);

	lock_sock(sk);
	sk_proto_close = ctx->sk_proto_close;
	kfree(ctx);

	release_sock(sk);
	sk_proto_close(sk, timeout);
}

static void process_deferq(struct work_struct *task_param)
{
	struct sk_buff *skb;
	struct chtls_dev *cdev = container_of(task_param,
				struct chtls_dev, deferq_task);

	spin_lock_bh(&cdev->deferq.lock);
	while ((skb = __skb_dequeue(&cdev->deferq)) != NULL) {
		spin_unlock_bh(&cdev->deferq.lock);
		DEFERRED_SKB_CB(skb)->handler(cdev, skb);
		spin_lock_bh(&cdev->deferq.lock);
	}
	spin_unlock_bh(&cdev->deferq.lock);
}

static int chtls_get_skb(struct chtls_dev *cdev)
{
	cdev->askb = alloc_skb(sizeof(struct tcphdr), GFP_KERNEL);
	if (!cdev->askb)
		return 1;

	skb_put(cdev->askb, sizeof(struct tcphdr));
	skb_reset_transport_header(cdev->askb);
	memset(cdev->askb->data, 0, cdev->askb->len);
	return 0;
}

static void *chtls_uld_add(const struct cxgb4_lld_info *info)
{
	struct chtls_dev *cdev;
	struct cxgb4_lld_info *lldi;
	int i;

	cdev = kzalloc(sizeof(*cdev) + info->nports *
		      (sizeof(struct net_device *)), GFP_KERNEL);
	if (!cdev)
		return NULL;

	lldi = kzalloc(sizeof(*lldi), GFP_KERNEL);
	if (!lldi) {
		kfree(cdev);
		return NULL;
	}

	if (chtls_get_skb(cdev)) {
		kfree(lldi);
		kfree(cdev);
		return NULL;
	}
	*lldi = *info;
	cdev->lldi = lldi;
	cdev->pdev = lldi->pdev;
	cdev->tids = lldi->tids;
	cdev->ports = (struct net_device **)(cdev + 1);
	cdev->ports = lldi->ports;
	cdev->mtus = lldi->mtus;
	cdev->tids = lldi->tids;
	cdev->pfvf = FW_VIID_PFN_G(cxgb4_port_viid(lldi->ports[0]))
			<< FW_VIID_PFN_S;

	for (i = 0; i < (1 << RSPQ_HASH_BITS); i++) {
		unsigned int size = 64 - sizeof(struct rsp_ctrl) - 8;

		cdev->rspq_skb_cache[i] = __alloc_skb(size,
						      gfp_any(), 0,
						      lldi->nodeid);
	}

	idr_init(&cdev->aidr);
	idr_init(&cdev->hwtid_idr);
	INIT_WORK(&cdev->deferq_task, process_deferq);
	spin_lock_init(&cdev->listen_lock);
	spin_lock_init(&cdev->idr_lock);
	spin_lock_init(&cdev->aidr_lock);
	cdev->send_page_order = min_t(uint, get_order(32768),
				      send_page_order);

	if (lldi->vr->key.size)
		chtls_init_kmap(cdev, lldi);

	mutex_lock(&cdev_mutex);
	list_add_tail(&cdev->list, &cdev_list);
	mutex_unlock(&cdev_mutex);

	return cdev;
}

static void chtls_free_uld(struct chtls_dev *cdev)
{
	int i;

	mutex_lock(&cdev_mutex);
	list_del(&cdev->list);
	mutex_unlock(&cdev_mutex);
	chtls_free_kmap(cdev);
	idr_destroy(&cdev->hwtid_idr);
	idr_destroy(&cdev->aidr);
	for (i = 0; i < (1 << RSPQ_HASH_BITS); i++) {
		kfree_skb(cdev->rspq_skb_cache[i]);
		cdev->rspq_skb_cache[i] = NULL;
	}
	kfree(cdev->lldi);
	if (cdev->askb)
		kfree_skb(cdev->askb);
	kfree(cdev);
}

static void chtls_free_all_uld(void)
{
	struct chtls_dev *cdev, *tmp;

	list_for_each_entry_safe(cdev, tmp, &cdev_list, list)
		chtls_free_uld(cdev);
}

static int chtls_uld_state_change(void *handle, enum cxgb4_state new_state)
{
	struct chtls_dev *cdev = handle;

	switch (new_state) {
	case CXGB4_STATE_UP:
		break;
	case CXGB4_STATE_DOWN:
		break;
	case CXGB4_STATE_START_RECOVERY:
		break;
	case CXGB4_STATE_DETACH:
		chtls_free_uld(cdev);
		break;
	}
	return 0;
}

struct sk_buff *copy_gl_to_skb_pkt(const struct pkt_gl *gl,
				   const __be64 *rsp,
				   u32 pktshift)
{
	struct sk_buff *skb;

	/* Allocate space for cpl_pass_accpet_req which will be synthesized by
	 * driver. Once driver synthesizes cpl_pass_accpet_req the skb will go
	 * through the regular cpl_pass_accept_req processing in TOM.
	 */
	skb = alloc_skb(gl->tot_len + sizeof(struct cpl_pass_accept_req)
			- pktshift, GFP_ATOMIC);
	if (unlikely(!skb))
		return NULL;
	__skb_put(skb, gl->tot_len + sizeof(struct cpl_pass_accept_req)
		   - pktshift);
	/* For now we will copy  cpl_rx_pkt in the skb */
	skb_copy_to_linear_data(skb, rsp, sizeof(struct cpl_rx_pkt));
	skb_copy_to_linear_data_offset(skb, sizeof(struct cpl_pass_accept_req)
				       , gl->va + pktshift,
				       gl->tot_len - pktshift);

	return skb;
}

int chtls_recv_packet(struct chtls_dev *cdev,
		      const struct pkt_gl *gl, const __be64 *rsp)
{
	unsigned int opcode = *(u8 *)rsp;
	struct sk_buff *skb;
	int ret;

	skb = copy_gl_to_skb_pkt(gl, rsp, cdev->lldi->sge_pktshift);
	if (!skb)
		return -ENOMEM;

	ret = chtls_handlers[opcode](cdev, skb);
	if (ret & CPL_RET_BUF_DONE)
		kfree_skb(skb);

	return 0;
}

static int chtls_recv_rsp(struct chtls_dev *cdev, const __be64 *rsp)
{
	unsigned int opcode = *(u8 *)rsp;
	unsigned int len = 64 - sizeof(struct rsp_ctrl) - 8;
	struct sk_buff *skb;
	unsigned long rspq_bin;
	int ret;

	rspq_bin = hash_ptr((void *)rsp, RSPQ_HASH_BITS);
	skb = cdev->rspq_skb_cache[rspq_bin];
	if (skb && !skb_is_nonlinear(skb) &&
	    !skb_shared(skb) && !skb_cloned(skb)) {
		refcount_inc(&skb->users);
		if (refcount_read(&skb->users) == 2) {
			__skb_trim(skb, 0);
			if (skb_tailroom(skb) >= len)
				goto copy_out;
		}
		refcount_dec(&skb->users);
	}
	skb = alloc_skb(len, GFP_ATOMIC);
	if (unlikely(!skb))
		return 1;

copy_out:
	__skb_put(skb, len);
	skb_copy_to_linear_data(skb, rsp, len);
	skb_reset_network_header(skb);
	skb_reset_transport_header(skb);
	ret = chtls_handlers[opcode](cdev, skb);

	if (ret & CPL_RET_BUF_DONE)
		kfree_skb(skb);
	return 0;
}

static int chtls_recv(struct chtls_dev *cdev,
		      struct sk_buff **skbs, const __be64 *rsp)
{
	struct sk_buff *skb = *skbs;
	unsigned int opcode = *(u8 *)rsp;
	int ret;

	__skb_push(skb, sizeof(struct rss_header));
	skb_copy_to_linear_data(skb, rsp, sizeof(struct rss_header));

	ret = chtls_handlers[opcode](cdev, skb);
	if (ret & CPL_RET_BUF_DONE)
		kfree_skb(skb);

	return 0;
}

static int chtls_uld_rx_handler(void *handle, const __be64 *rsp,
				const struct pkt_gl *gl)
{
	struct chtls_dev *cdev = handle;
	struct sk_buff *skb;
	unsigned int opcode = *(u8 *)rsp;

	if (unlikely(opcode == CPL_RX_PKT)) {
		if (chtls_recv_packet(cdev, gl, rsp) < 0)
			goto nomem;
		return 0;
	}

	if (!gl)
		return chtls_recv_rsp(cdev, rsp);

#define RX_PULL_LEN 128
	skb = cxgb4_pktgl_to_skb(gl, RX_PULL_LEN, RX_PULL_LEN);
	if (unlikely(!skb))
		goto nomem;
	chtls_recv(cdev, &skb, rsp);
	return 0;

nomem:
	return 1;
}

static int do_chtls_getsockopt(struct sock *sk, char __user *optval,
			       int __user *optlen)
{
	struct tls_crypto_info crypto_info;

	crypto_info.version = TLS_1_2_VERSION;
	if (copy_to_user(optval, &crypto_info, sizeof(struct tls_crypto_info)))
		return -EFAULT;
	return 0;
}

static int chtls_getsockopt(struct sock *sk, int level, int optname,
			    char __user *optval, int __user *optlen)
{
	struct tls_context *ctx = tls_get_ctx(sk);

	if (level != SOL_TLS)
		return ctx->getsockopt(sk, level, optname, optval, optlen);

	return do_chtls_getsockopt(sk, optval, optlen);
}

static int do_chtls_setsockopt(struct sock *sk, int optname,
			       char __user *optval, unsigned int optlen)
{
	struct chtls_sock *csk = rcu_dereference_sk_user_data(sk);
	struct tls_crypto_info *crypto_info, tmp_crypto_info;
	int keylen;
	int rc = 0;

	if (!optval || optlen < sizeof(*crypto_info)) {
		rc = -EINVAL;
		goto out;
	}

	rc = copy_from_user(&tmp_crypto_info, optval, sizeof(*crypto_info));
	if (rc) {
		rc = -EFAULT;
		goto out;
	}

	/* check version */
	if (tmp_crypto_info.version != TLS_1_2_VERSION) {
		rc = -ENOTSUPP;
		goto out;
	}

	crypto_info = (struct tls_crypto_info *)&csk->tlshws.crypto_info;

	switch (tmp_crypto_info.cipher_type) {
	case TLS_CIPHER_AES_GCM_128: {
		rc = copy_from_user(crypto_info, optval,
				    sizeof(struct
					   tls12_crypto_info_aes_gcm_128));

		if (rc) {
			rc = -EFAULT;
			goto out;
		}

		keylen = TLS_CIPHER_AES_GCM_128_KEY_SIZE;
		chtls_setkey(csk, crypto_info, keylen, optname);
		break;
	}
	default:
		rc = -EINVAL;
		goto out;
	}
out:
	return rc;
}

static int chtls_setsockopt(struct sock *sk, int level, int optname,
			    char __user *optval, unsigned int optlen)
{
	struct tls_context *ctx = tls_get_ctx(sk);

	if (level != SOL_TLS)
		return ctx->setsockopt(sk, level, optname, optval, optlen);

	return do_chtls_setsockopt(sk, optname, optval, optlen);
}

static int chtls_init(struct sock *sk)
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
	sk->sk_prot = &chtls_base_prot;
out:
	return rc;
}

static struct cxgb4_uld_info chtls_uld_info = {
	.name = DRV_NAME,
	.nrxq = MAX_ULD_QSETS,
	.ntxq = MAX_ULD_QSETS,
	.rxq_size = 1024,
	.add = chtls_uld_add,
	.state_change = chtls_uld_state_change,
	.rx_handler = chtls_uld_rx_handler,
};

static struct tcp_ulp_ops tcp_chtls_ulp_ops __read_mostly = {
	.name                   = "chtls",
	.owner                  = THIS_MODULE,
	.init                   = chtls_init,
};

void chtls_install_cpl_ops(struct sock *sk)
{
	sk->sk_prot = &chtls_cpl_prot;
}

static void __init chtls_init_ulp_ops(void)
{
	chtls_base_prot			= tcp_prot;
	chtls_base_prot.hash		= chtls_hash;
	chtls_base_prot.unhash		= chtls_unhash;
	chtls_base_prot.close		= chtls_lsk_close;

	chtls_cpl_prot			= chtls_base_prot;
	chtls_init_rsk_ops(&chtls_cpl_prot, &chtls_rsk_ops,
			   &tcp_prot, PF_INET);
	get_tcp_symbol();
	chtls_cpl_prot.close		= chtls_close;
	chtls_cpl_prot.disconnect	= chtls_disconnect;
	chtls_cpl_prot.destroy		= chtls_destroy_sock;
	chtls_cpl_prot.shutdown		= chtls_shutdown;
	chtls_cpl_prot.sendmsg		= chtls_sendmsg;
	chtls_cpl_prot.recvmsg		= chtls_recvmsg;
	chtls_cpl_prot.sendpage		= chtls_sendpage;
	chtls_cpl_prot.setsockopt	= chtls_setsockopt;
	chtls_cpl_prot.getsockopt	= chtls_getsockopt;
}

static int __init chtls_register(void)
{
	chtls_init_ulp_ops();
	register_listen_notifier(&listen_notifier);
	cxgb4_register_uld(CXGB4_ULD_TLS, &chtls_uld_info);
	tcp_register_ulp(&tcp_chtls_ulp_ops);
	return 0;
}

static void __exit chtls_unregister(void)
{
	unregister_listen_notifier(&listen_notifier);
	tcp_unregister_ulp(&tcp_chtls_ulp_ops);
	chtls_free_all_uld();
	cxgb4_unregister_uld(CXGB4_ULD_TLS);
}

module_init(chtls_register);
module_exit(chtls_unregister);

MODULE_DESCRIPTION("Chelsio TLS Inline driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chelsio Communications");
MODULE_VERSION(DRV_VERSION);
