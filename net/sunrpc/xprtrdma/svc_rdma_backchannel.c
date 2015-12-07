/*
 * Copyright (c) 2015 Oracle.  All rights reserved.
 *
 * Support for backward direction RPCs on RPC/RDMA (server-side).
 */

#include <linux/sunrpc/svc_rdma.h>
#include "xprt_rdma.h"

#define RPCDBG_FACILITY	RPCDBG_SVCXPRT

#undef SVCRDMA_BACKCHANNEL_DEBUG

int
svc_rdma_handle_bc_reply(struct rpc_xprt *xprt, struct rpcrdma_msg *rmsgp,
			 struct xdr_buf *rcvbuf)
{
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(xprt);
	struct kvec *dst, *src = &rcvbuf->head[0];
	struct rpc_rqst *req;
	unsigned long cwnd;
	u32 credits;
	size_t len;
	__be32 xid;
	__be32 *p;
	int ret;

	p = (__be32 *)src->iov_base;
	len = src->iov_len;
	xid = rmsgp->rm_xid;

#ifdef SVCRDMA_BACKCHANNEL_DEBUG
	pr_info("%s: xid=%08x, length=%zu\n",
		__func__, be32_to_cpu(xid), len);
	pr_info("%s: RPC/RDMA: %*ph\n",
		__func__, (int)RPCRDMA_HDRLEN_MIN, rmsgp);
	pr_info("%s:      RPC: %*ph\n",
		__func__, (int)len, p);
#endif

	ret = -EAGAIN;
	if (src->iov_len < 24)
		goto out_shortreply;

	spin_lock_bh(&xprt->transport_lock);
	req = xprt_lookup_rqst(xprt, xid);
	if (!req)
		goto out_notfound;

	dst = &req->rq_private_buf.head[0];
	memcpy(&req->rq_private_buf, &req->rq_rcv_buf, sizeof(struct xdr_buf));
	if (dst->iov_len < len)
		goto out_unlock;
	memcpy(dst->iov_base, p, len);

	credits = be32_to_cpu(rmsgp->rm_credit);
	if (credits == 0)
		credits = 1;	/* don't deadlock */
	else if (credits > r_xprt->rx_buf.rb_bc_max_requests)
		credits = r_xprt->rx_buf.rb_bc_max_requests;

	cwnd = xprt->cwnd;
	xprt->cwnd = credits << RPC_CWNDSHIFT;
	if (xprt->cwnd > cwnd)
		xprt_release_rqst_cong(req->rq_task);

	ret = 0;
	xprt_complete_rqst(req->rq_task, rcvbuf->len);
	rcvbuf->len = 0;

out_unlock:
	spin_unlock_bh(&xprt->transport_lock);
out:
	return ret;

out_shortreply:
	dprintk("svcrdma: short bc reply: xprt=%p, len=%zu\n",
		xprt, src->iov_len);
	goto out;

out_notfound:
	dprintk("svcrdma: unrecognized bc reply: xprt=%p, xid=%08x\n",
		xprt, be32_to_cpu(xid));

	goto out_unlock;
}

/* Server-side transport endpoint wants a whole page for its send
 * buffer. The client RPC code constructs the RPC header in this
 * buffer before it invokes ->send_request.
 *
 * Returns NULL if there was a temporary allocation failure.
 */
static void *
xprt_rdma_bc_allocate(struct rpc_task *task, size_t size)
{
	struct rpc_rqst *rqst = task->tk_rqstp;
	struct svc_rdma_op_ctxt *ctxt;
	struct svcxprt_rdma *rdma;
	struct svc_xprt *sxprt;
	struct page *page;

	/* Prevent an infinite loop: don't return NULL */
	if (size > PAGE_SIZE)
		WARN_ONCE(1, "svcrdma: bc buffer request too large (size %zu)\n",
			  size);

	page = alloc_page(RPCRDMA_DEF_GFP);
	if (!page)
		return NULL;

	sxprt = rqst->rq_xprt->bc_xprt;
	rdma = container_of(sxprt, struct svcxprt_rdma, sc_xprt);
	ctxt = svc_rdma_get_context(rdma);
	if (!ctxt) {
		put_page(page);
		return NULL;
	}

	rqst->rq_privdata = ctxt;
	ctxt->pages[0] = page;
	ctxt->count = 1;
	return page_address(page);
}

static void
xprt_rdma_bc_free(void *buffer)
{
	/* No-op: ctxt and page have already been freed. */
}

static int
rpcrdma_bc_send_request(struct svcxprt_rdma *rdma, struct rpc_rqst *rqst)
{
	struct rpc_xprt *xprt = rqst->rq_xprt;
	struct rpcrdma_xprt *r_xprt = rpcx_to_rdmax(xprt);
	struct rpcrdma_msg *headerp = (struct rpcrdma_msg *)rqst->rq_buffer;
	struct svc_rdma_op_ctxt *ctxt;
	int rc;

	/* Space in the send buffer for an RPC/RDMA header is reserved
	 * via xprt->tsh_size */
	headerp->rm_xid = rqst->rq_xid;
	headerp->rm_vers = rpcrdma_version;
	headerp->rm_credit = cpu_to_be32(r_xprt->rx_buf.rb_bc_max_requests);
	headerp->rm_type = rdma_msg;
	headerp->rm_body.rm_chunks[0] = xdr_zero;
	headerp->rm_body.rm_chunks[1] = xdr_zero;
	headerp->rm_body.rm_chunks[2] = xdr_zero;

#ifdef SVCRDMA_BACKCHANNEL_DEBUG
	pr_info("%s: %*ph\n", __func__, 64, rqst->rq_buffer);
#endif

	ctxt = (struct svc_rdma_op_ctxt *)rqst->rq_privdata;
	rc = svc_rdma_bc_post_send(rdma, ctxt, &rqst->rq_snd_buf);
	if (rc)
		goto drop_connection;
	return rc;

drop_connection:
	dprintk("svcrdma: failed to send bc call\n");
	svc_rdma_put_context(ctxt, 1);
	xprt_disconnect_done(xprt);
	return -ENOTCONN;
}

/* Send an RPC call on the passive end of a transport
 * connection.
 */
static int
xprt_rdma_bc_send_request(struct rpc_task *task)
{
	struct rpc_rqst *rqst = task->tk_rqstp;
	struct svc_xprt *sxprt = rqst->rq_xprt->bc_xprt;
	struct svcxprt_rdma *rdma;
	u32 len;

	dprintk("svcrdma: sending bc call with xid: %08x\n",
		be32_to_cpu(rqst->rq_xid));

	if (!mutex_trylock(&sxprt->xpt_mutex)) {
		rpc_sleep_on(&sxprt->xpt_bc_pending, task, NULL);
		if (!mutex_trylock(&sxprt->xpt_mutex))
			return -EAGAIN;
		rpc_wake_up_queued_task(&sxprt->xpt_bc_pending, task);
	}

	len = -ENOTCONN;
	rdma = container_of(sxprt, struct svcxprt_rdma, sc_xprt);
	if (!test_bit(XPT_DEAD, &sxprt->xpt_flags))
		len = rpcrdma_bc_send_request(rdma, rqst);

	mutex_unlock(&sxprt->xpt_mutex);

	if (len < 0)
		return len;
	return 0;
}

static void
xprt_rdma_bc_close(struct rpc_xprt *xprt)
{
	dprintk("svcrdma: %s: xprt %p\n", __func__, xprt);
}

static void
xprt_rdma_bc_put(struct rpc_xprt *xprt)
{
	dprintk("svcrdma: %s: xprt %p\n", __func__, xprt);

	xprt_free(xprt);
	module_put(THIS_MODULE);
}

static struct rpc_xprt_ops xprt_rdma_bc_procs = {
	.reserve_xprt		= xprt_reserve_xprt_cong,
	.release_xprt		= xprt_release_xprt_cong,
	.alloc_slot		= xprt_alloc_slot,
	.release_request	= xprt_release_rqst_cong,
	.buf_alloc		= xprt_rdma_bc_allocate,
	.buf_free		= xprt_rdma_bc_free,
	.send_request		= xprt_rdma_bc_send_request,
	.set_retrans_timeout	= xprt_set_retrans_timeout_def,
	.close			= xprt_rdma_bc_close,
	.destroy		= xprt_rdma_bc_put,
	.print_stats		= xprt_rdma_print_stats
};

static const struct rpc_timeout xprt_rdma_bc_timeout = {
	.to_initval = 60 * HZ,
	.to_maxval = 60 * HZ,
};

/* It shouldn't matter if the number of backchannel session slots
 * doesn't match the number of RPC/RDMA credits. That just means
 * one or the other will have extra slots that aren't used.
 */
static struct rpc_xprt *
xprt_setup_rdma_bc(struct xprt_create *args)
{
	struct rpc_xprt *xprt;
	struct rpcrdma_xprt *new_xprt;

	if (args->addrlen > sizeof(xprt->addr)) {
		dprintk("RPC:       %s: address too large\n", __func__);
		return ERR_PTR(-EBADF);
	}

	xprt = xprt_alloc(args->net, sizeof(*new_xprt),
			  RPCRDMA_MAX_BC_REQUESTS,
			  RPCRDMA_MAX_BC_REQUESTS);
	if (xprt == NULL) {
		dprintk("RPC:       %s: couldn't allocate rpc_xprt\n",
			__func__);
		return ERR_PTR(-ENOMEM);
	}

	xprt->timeout = &xprt_rdma_bc_timeout;
	xprt_set_bound(xprt);
	xprt_set_connected(xprt);
	xprt->bind_timeout = RPCRDMA_BIND_TO;
	xprt->reestablish_timeout = RPCRDMA_INIT_REEST_TO;
	xprt->idle_timeout = RPCRDMA_IDLE_DISC_TO;

	xprt->prot = XPRT_TRANSPORT_BC_RDMA;
	xprt->tsh_size = RPCRDMA_HDRLEN_MIN / sizeof(__be32);
	xprt->ops = &xprt_rdma_bc_procs;

	memcpy(&xprt->addr, args->dstaddr, args->addrlen);
	xprt->addrlen = args->addrlen;
	xprt_rdma_format_addresses(xprt, (struct sockaddr *)&xprt->addr);
	xprt->resvport = 0;

	xprt->max_payload = xprt_rdma_max_inline_read;

	new_xprt = rpcx_to_rdmax(xprt);
	new_xprt->rx_buf.rb_bc_max_requests = xprt->max_reqs;

	xprt_get(xprt);
	args->bc_xprt->xpt_bc_xprt = xprt;
	xprt->bc_xprt = args->bc_xprt;

	if (!try_module_get(THIS_MODULE))
		goto out_fail;

	/* Final put for backchannel xprt is in __svc_rdma_free */
	xprt_get(xprt);
	return xprt;

out_fail:
	xprt_rdma_free_addresses(xprt);
	args->bc_xprt->xpt_bc_xprt = NULL;
	xprt_put(xprt);
	xprt_free(xprt);
	return ERR_PTR(-EINVAL);
}

struct xprt_class xprt_rdma_bc = {
	.list			= LIST_HEAD_INIT(xprt_rdma_bc.list),
	.name			= "rdma backchannel",
	.owner			= THIS_MODULE,
	.ident			= XPRT_TRANSPORT_BC_RDMA,
	.setup			= xprt_setup_rdma_bc,
};
