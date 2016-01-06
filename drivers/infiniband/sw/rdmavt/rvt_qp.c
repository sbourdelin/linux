/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *		- Redistributions of source code must retain the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer.
 *
 *		- Redistributions in binary form must reproduce the above
 *		  copyright notice, this list of conditions and the following
 *		  disclaimer in the documentation and/or other materials
 *		  provided with the distribution.
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

#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/sched.h>

#include "rvt_loc.h"
#include "rvt_queue.h"
#include "rvt_task.h"

char *rvt_qp_state_name[] = {
	[QP_STATE_RESET]	= "RESET",
	[QP_STATE_INIT]		= "INIT",
	[QP_STATE_READY]	= "READY",
	[QP_STATE_DRAIN]	= "DRAIN",
	[QP_STATE_DRAINED]	= "DRAINED",
	[QP_STATE_ERROR]	= "ERROR",
};

static int rvt_qp_chk_cap(struct rvt_dev *rvt, struct ib_qp_cap *cap,
			  int has_srq)
{
	if (cap->max_send_wr > rvt->attr.max_qp_wr) {
		pr_warn("invalid send wr = %d > %d\n",
			cap->max_send_wr, rvt->attr.max_qp_wr);
		goto err1;
	}

	if (cap->max_send_sge > rvt->attr.max_sge) {
		pr_warn("invalid send sge = %d > %d\n",
			cap->max_send_sge, rvt->attr.max_sge);
		goto err1;
	}

	if (!has_srq) {
		if (cap->max_recv_wr > rvt->attr.max_qp_wr) {
			pr_warn("invalid recv wr = %d > %d\n",
				cap->max_recv_wr, rvt->attr.max_qp_wr);
			goto err1;
		}

		if (cap->max_recv_sge > rvt->attr.max_sge) {
			pr_warn("invalid recv sge = %d > %d\n",
				cap->max_recv_sge, rvt->attr.max_sge);
			goto err1;
		}
	}

	if (cap->max_inline_data > rvt->max_inline_data) {
		pr_warn("invalid max inline data = %d > %d\n",
			cap->max_inline_data, rvt->max_inline_data);
		goto err1;
	}

	return 0;

err1:
	return -EINVAL;
}

int rvt_qp_chk_init(struct rvt_dev *rvt, struct ib_qp_init_attr *init)
{
	struct ib_qp_cap *cap = &init->cap;
	struct rvt_port *port;
	int port_num = init->port_num;

	if (!init->recv_cq || !init->send_cq) {
		pr_warn("missing cq\n");
		goto err1;
	}

	if (rvt_qp_chk_cap(rvt, cap, !!init->srq))
		goto err1;

	if (init->qp_type == IB_QPT_SMI || init->qp_type == IB_QPT_GSI) {
		if (port_num < 1 || port_num > rvt->num_ports) {
			pr_warn("invalid port = %d\n", port_num);
			goto err1;
		}

		port = &rvt->port[port_num - 1];

		if (init->qp_type == IB_QPT_SMI && port->qp_smi_index) {
			pr_warn("SMI QP exists for port %d\n", port_num);
			goto err1;
		}

		if (init->qp_type == IB_QPT_GSI && port->qp_gsi_index) {
			pr_warn("GSI QP exists for port %d\n", port_num);
			goto err1;
		}
	}

	return 0;

err1:
	return -EINVAL;
}

static int alloc_rd_atomic_resources(struct rvt_qp *qp, unsigned int n)
{
	qp->resp.res_head = 0;
	qp->resp.res_tail = 0;
	qp->resp.resources = kcalloc(n, sizeof(struct resp_res), GFP_KERNEL);

	if (!qp->resp.resources)
		return -ENOMEM;

	return 0;
}

static void free_rd_atomic_resources(struct rvt_qp *qp)
{
	if (qp->resp.resources) {
		int i;

		for (i = 0; i < qp->attr.max_rd_atomic; i++) {
			struct resp_res *res = &qp->resp.resources[i];

			free_rd_atomic_resource(qp, res);
		}
		kfree(qp->resp.resources);
		qp->resp.resources = NULL;
	}
}

void free_rd_atomic_resource(struct rvt_qp *qp, struct resp_res *res)
{
	if (res->type == RVT_ATOMIC_MASK) {
		rvt_drop_ref(qp);
		kfree_skb(res->atomic.skb);
	} else if (res->type == RVT_READ_MASK) {
		if (res->read.mr)
			rvt_drop_ref(res->read.mr);
	}
	res->type = 0;
}

static void cleanup_rd_atomic_resources(struct rvt_qp *qp)
{
	int i;
	struct resp_res *res;

	if (qp->resp.resources) {
		for (i = 0; i < qp->attr.max_rd_atomic; i++) {
			res = &qp->resp.resources[i];
			free_rd_atomic_resource(qp, res);
		}
	}
}

static void rvt_qp_init_misc(struct rvt_dev *rvt, struct rvt_qp *qp,
			     struct ib_qp_init_attr *init)
{
	struct rvt_port *port;
	u32 qpn;

	qp->sq_sig_type		= init->sq_sig_type;
	qp->attr.path_mtu	= 1;
	qp->mtu			= 256;

	qpn			= qp->pelem.index;
	port			= &rvt->port[init->port_num - 1];

	switch (init->qp_type) {
	case IB_QPT_SMI:
		qp->ibqp.qp_num		= 0;
		port->qp_smi_index	= qpn;
		qp->attr.port_num	= init->port_num;
		break;

	case IB_QPT_GSI:
		qp->ibqp.qp_num		= 1;
		port->qp_gsi_index	= qpn;
		qp->attr.port_num	= init->port_num;
		break;

	default:
		qp->ibqp.qp_num		= qpn;
		break;
	}

	INIT_LIST_HEAD(&qp->grp_list);

	skb_queue_head_init(&qp->send_pkts);

	spin_lock_init(&qp->grp_lock);
	spin_lock_init(&qp->state_lock);

	atomic_set(&qp->ssn, 0);
	atomic_set(&qp->skb_out, 0);
}

static int rvt_qp_init_req(struct rvt_dev *rdev, struct rvt_qp *qp,
			   struct ib_qp_init_attr *init,
			   struct ib_ucontext *context, struct ib_udata *udata)
{
	int err;
	int wqe_size;

	err = rdev->ifc_ops->create_flow(rdev, &qp->flow, &qp);
	if (err)
		return err;

	qp->sq.max_wr		= init->cap.max_send_wr;
	qp->sq.max_sge		= init->cap.max_send_sge;
	qp->sq.max_inline	= init->cap.max_inline_data;

	wqe_size = max_t(int, sizeof(struct rvt_send_wqe) +
			 qp->sq.max_sge * sizeof(struct ib_sge),
			 sizeof(struct rvt_send_wqe) +
			 qp->sq.max_inline);

	qp->sq.queue		= rvt_queue_init(rdev,
						 &qp->sq.max_wr,
						 wqe_size);
	if (!qp->sq.queue)
		return -ENOMEM;

	err = do_mmap_info(rdev, udata, true,
			   context, qp->sq.queue->buf,
			   qp->sq.queue->buf_size, &qp->sq.queue->ip);

	if (err) {
		kvfree(qp->sq.queue->buf);
		kfree(qp->sq.queue);
		return err;
	}

	qp->req.wqe_index	= producer_index(qp->sq.queue);
	qp->req.state		= QP_STATE_RESET;
	qp->req.opcode		= -1;
	qp->comp.opcode		= -1;

	spin_lock_init(&qp->sq.sq_lock);
	skb_queue_head_init(&qp->req_pkts);

	rvt_init_task(rdev, &qp->req.task, qp,
		      rvt_requester, "req");
	rvt_init_task(rdev, &qp->comp.task, qp,
		      rvt_completer, "comp");

	init_timer(&qp->rnr_nak_timer);
	qp->rnr_nak_timer.function = rnr_nak_timer;
	qp->rnr_nak_timer.data = (unsigned long)qp;

	init_timer(&qp->retrans_timer);
	qp->retrans_timer.function = retransmit_timer;
	qp->retrans_timer.data = (unsigned long)qp;
	qp->qp_timeout_jiffies = 0; /* Can't be set for UD/UC in modify_qp */

	return 0;
}

static int rvt_qp_init_resp(struct rvt_dev *rdev, struct rvt_qp *qp,
			    struct ib_qp_init_attr *init,
			    struct ib_ucontext *context, struct ib_udata *udata)
{
	int err;
	int wqe_size;

	if (!qp->srq) {
		qp->rq.max_wr		= init->cap.max_recv_wr;
		qp->rq.max_sge		= init->cap.max_recv_sge;

		wqe_size = rcv_wqe_size(qp->rq.max_sge);

		pr_debug("max_wr = %d, max_sge = %d, wqe_size = %d\n",
			 qp->rq.max_wr, qp->rq.max_sge, wqe_size);

		qp->rq.queue		= rvt_queue_init(rdev,
						&qp->rq.max_wr,
						wqe_size);
		if (!qp->rq.queue)
			return -ENOMEM;

		err = do_mmap_info(rdev, udata, false, context,
				   qp->rq.queue->buf,
				   qp->rq.queue->buf_size,
				   &qp->rq.queue->ip);
		if (err) {
			kvfree(qp->rq.queue->buf);
			kfree(qp->rq.queue);
			return err;
		}
	}

	spin_lock_init(&qp->rq.producer_lock);
	spin_lock_init(&qp->rq.consumer_lock);

	skb_queue_head_init(&qp->resp_pkts);

	rvt_init_task(rdev, &qp->resp.task, qp,
		      rvt_responder, "resp");

	qp->resp.opcode		= OPCODE_NONE;
	qp->resp.msn		= 0;
	qp->resp.state		= QP_STATE_RESET;

	return 0;
}

/* called by the create qp verb */
int rvt_qp_from_init(struct rvt_dev *rdev, struct rvt_qp *qp, struct rvt_pd *pd,
		     struct ib_qp_init_attr *init, struct ib_udata *udata,
		     struct ib_pd *ibpd)
{
	int err;
	struct rvt_cq *rcq = to_rcq(init->recv_cq);
	struct rvt_cq *scq = to_rcq(init->send_cq);
	struct rvt_srq *srq = init->srq ? to_rsrq(init->srq) : NULL;
	struct ib_ucontext *context = udata ? ibpd->uobject->context : NULL;

	rvt_add_ref(pd);
	rvt_add_ref(rcq);
	rvt_add_ref(scq);
	if (srq)
		rvt_add_ref(srq);

	qp->pd			= pd;
	qp->rcq			= rcq;
	qp->scq			= scq;
	qp->srq			= srq;

	rvt_qp_init_misc(rdev, qp, init);

	err = rvt_qp_init_req(rdev, qp, init, context, udata);
	if (err)
		goto err1;

	err = rvt_qp_init_resp(rdev, qp, init, context, udata);
	if (err)
		goto err2;

	qp->attr.qp_state = IB_QPS_RESET;
	qp->valid = 1;

	return 0;

err2:
	rvt_queue_cleanup(qp->sq.queue);
err1:
	if (srq)
		rvt_drop_ref(srq);
	rvt_drop_ref(scq);
	rvt_drop_ref(rcq);
	rvt_drop_ref(pd);

	return err;
}

/* called by the query qp verb */
int rvt_qp_to_init(struct rvt_qp *qp, struct ib_qp_init_attr *init)
{
	init->event_handler		= qp->ibqp.event_handler;
	init->qp_context		= qp->ibqp.qp_context;
	init->send_cq			= qp->ibqp.send_cq;
	init->recv_cq			= qp->ibqp.recv_cq;
	init->srq			= qp->ibqp.srq;

	init->cap.max_send_wr		= qp->sq.max_wr;
	init->cap.max_send_sge		= qp->sq.max_sge;
	init->cap.max_inline_data	= qp->sq.max_inline;

	if (!qp->srq) {
		init->cap.max_recv_wr		= qp->rq.max_wr;
		init->cap.max_recv_sge		= qp->rq.max_sge;
	}

	init->sq_sig_type		= qp->sq_sig_type;

	init->qp_type			= qp->ibqp.qp_type;
	init->port_num			= 1;

	return 0;
}

/* called by the modify qp verb, this routine checks all the parameters before
 * making any changes
 */
int rvt_qp_chk_attr(struct rvt_dev *rvt, struct rvt_qp *qp,
		    struct ib_qp_attr *attr, int mask)
{
	enum ib_qp_state cur_state = (mask & IB_QP_CUR_STATE) ?
					attr->cur_qp_state : qp->attr.qp_state;
	enum ib_qp_state new_state = (mask & IB_QP_STATE) ?
					attr->qp_state : cur_state;

	if (!ib_modify_qp_is_ok(cur_state, new_state, qp_type(qp), mask,
				IB_LINK_LAYER_ETHERNET)) {
		pr_warn("invalid mask or state for qp\n");
		goto err1;
	}

	if (mask & IB_QP_STATE) {
		if (cur_state == IB_QPS_SQD) {
			if (qp->req.state == QP_STATE_DRAIN &&
			    new_state != IB_QPS_ERR)
				goto err1;
		}
	}

	if (mask & IB_QP_PORT) {
		if (attr->port_num < 1 || attr->port_num > rvt->num_ports) {
			pr_warn("invalid port %d\n", attr->port_num);
			goto err1;
		}
	}

	if (mask & IB_QP_CAP && rvt_qp_chk_cap(rvt, &attr->cap, !!qp->srq))
		goto err1;

	if (mask & IB_QP_AV && rvt_av_chk_attr(rvt, &attr->ah_attr))
		goto err1;

	if (mask & IB_QP_ALT_PATH && rvt_av_chk_attr(rvt, &attr->alt_ah_attr))
		goto err1;

	if (mask & IB_QP_PATH_MTU) {
		struct rvt_port *port = &rvt->port[qp->attr.port_num - 1];
		enum ib_mtu max_mtu = port->attr.max_mtu;
		enum ib_mtu mtu = attr->path_mtu;

		if (mtu > max_mtu) {
			pr_debug("invalid mtu (%d) > (%d)\n",
				 ib_mtu_enum_to_int(mtu),
				 ib_mtu_enum_to_int(max_mtu));
			goto err1;
		}
	}

	if (mask & IB_QP_MAX_QP_RD_ATOMIC) {
		if (attr->max_rd_atomic > rvt->attr.max_qp_rd_atom) {
			pr_warn("invalid max_rd_atomic %d > %d\n",
				attr->max_rd_atomic,
				rvt->attr.max_qp_rd_atom);
			goto err1;
		}
	}

	if (mask & IB_QP_TIMEOUT) {
		if (attr->timeout > 31) {
			pr_warn("invalid QP timeout %d > 31\n",
				attr->timeout);
			goto err1;
		}
	}

	return 0;

err1:
	return -EINVAL;
}

/* move the qp to the reset state */
static void rvt_qp_reset(struct rvt_qp *qp)
{
	/* stop tasks from running */
	rvt_disable_task(&qp->resp.task);

	/* stop request/comp */
	if (qp_type(qp) == IB_QPT_RC)
		rvt_disable_task(&qp->comp.task);
	rvt_disable_task(&qp->req.task);

	/* move qp to the reset state */
	qp->req.state = QP_STATE_RESET;
	qp->resp.state = QP_STATE_RESET;

	/* let state machines reset themselves drain work and packet queues
	 * etc.
	 */
	__rvt_do_task(&qp->resp.task);

	if (qp->sq.queue) {
		__rvt_do_task(&qp->comp.task);
		__rvt_do_task(&qp->req.task);
	}

	/* cleanup attributes */
	atomic_set(&qp->ssn, 0);
	qp->req.opcode = -1;
	qp->req.need_retry = 0;
	qp->req.noack_pkts = 0;
	qp->resp.msn = 0;
	qp->resp.opcode = -1;
	qp->resp.drop_msg = 0;
	qp->resp.goto_error = 0;
	qp->resp.sent_psn_nak = 0;

	if (qp->resp.mr) {
		rvt_drop_ref(qp->resp.mr);
		qp->resp.mr = NULL;
	}

	cleanup_rd_atomic_resources(qp);

	/* reenable tasks */
	rvt_enable_task(&qp->resp.task);

	if (qp->sq.queue) {
		if (qp_type(qp) == IB_QPT_RC)
			rvt_enable_task(&qp->comp.task);

		rvt_enable_task(&qp->req.task);
	}
}

/* drain the send queue */
static void rvt_qp_drain(struct rvt_qp *qp)
{
	if (qp->sq.queue) {
		if (qp->req.state != QP_STATE_DRAINED) {
			qp->req.state = QP_STATE_DRAIN;
			if (qp_type(qp) == IB_QPT_RC)
				rvt_run_task(&qp->comp.task, 1);
			else
				__rvt_do_task(&qp->comp.task);
			rvt_run_task(&qp->req.task, 1);
		}
	}
}

/* move the qp to the error state */
void rvt_qp_error(struct rvt_qp *qp)
{
	qp->req.state = QP_STATE_ERROR;
	qp->resp.state = QP_STATE_ERROR;

	/* drain work and packet queues */
	rvt_run_task(&qp->resp.task, 1);

	if (qp_type(qp) == IB_QPT_RC)
		rvt_run_task(&qp->comp.task, 1);
	else
		__rvt_do_task(&qp->comp.task);
	rvt_run_task(&qp->req.task, 1);
}

/* called by the modify qp verb */
int rvt_qp_from_attr(struct rvt_qp *qp, struct ib_qp_attr *attr, int mask,
		     struct ib_udata *udata)
{
	int err;
	struct rvt_dev *rvt = to_rdev(qp->ibqp.device);
	union ib_gid sgid;
	struct ib_gid_attr sgid_attr;

	if (mask & IB_QP_MAX_QP_RD_ATOMIC) {
		int max_rd_atomic = __roundup_pow_of_two(attr->max_rd_atomic);

		free_rd_atomic_resources(qp);

		err = alloc_rd_atomic_resources(qp, max_rd_atomic);
		if (err)
			return err;

		qp->attr.max_rd_atomic = max_rd_atomic;
		atomic_set(&qp->req.rd_atomic, max_rd_atomic);
	}

	if (mask & IB_QP_CUR_STATE)
		qp->attr.cur_qp_state = attr->qp_state;

	if (mask & IB_QP_EN_SQD_ASYNC_NOTIFY)
		qp->attr.en_sqd_async_notify = attr->en_sqd_async_notify;

	if (mask & IB_QP_ACCESS_FLAGS)
		qp->attr.qp_access_flags = attr->qp_access_flags;

	if (mask & IB_QP_PKEY_INDEX)
		qp->attr.pkey_index = attr->pkey_index;

	if (mask & IB_QP_PORT)
		qp->attr.port_num = attr->port_num;

	if (mask & IB_QP_QKEY)
		qp->attr.qkey = attr->qkey;

	if (mask & IB_QP_AV) {
		rcu_read_lock();
		ib_get_cached_gid(&rvt->ib_dev, 1,
				  attr->ah_attr.grh.sgid_index, &sgid,
				  &sgid_attr);
		rcu_read_unlock();
		rvt_av_from_attr(rvt, attr->port_num, &qp->pri_av,
				 &attr->ah_attr);
		rvt_av_fill_ip_info(rvt, &qp->pri_av, &attr->ah_attr,
				    &sgid_attr, &sgid);
	}

	if (mask & IB_QP_ALT_PATH) {
		rcu_read_lock();
		ib_get_cached_gid(&rvt->ib_dev, 1,
				  attr->alt_ah_attr.grh.sgid_index, &sgid,
				  &sgid_attr);
		rcu_read_unlock();

		rvt_av_from_attr(rvt, attr->alt_port_num, &qp->alt_av,
				 &attr->alt_ah_attr);
		rvt_av_fill_ip_info(rvt, &qp->alt_av, &attr->alt_ah_attr,
				    &sgid_attr, &sgid);
		qp->attr.alt_port_num = attr->alt_port_num;
		qp->attr.alt_pkey_index = attr->alt_pkey_index;
		qp->attr.alt_timeout = attr->alt_timeout;
	}

	if (mask & IB_QP_PATH_MTU) {
		qp->attr.path_mtu = attr->path_mtu;
		qp->mtu = ib_mtu_enum_to_int(attr->path_mtu);
	}

	if (mask & IB_QP_TIMEOUT) {
		qp->attr.timeout = attr->timeout;
		if (attr->timeout == 0) {
			qp->qp_timeout_jiffies = 0;
		} else {
			int j = usecs_to_jiffies(4ULL << attr->timeout);

			qp->qp_timeout_jiffies = j ? j : 1;
		}
	}

	if (mask & IB_QP_RETRY_CNT) {
		qp->attr.retry_cnt = attr->retry_cnt;
		qp->comp.retry_cnt = attr->retry_cnt;
		pr_debug("set retry count = %d\n", attr->retry_cnt);
	}

	if (mask & IB_QP_RNR_RETRY) {
		qp->attr.rnr_retry = attr->rnr_retry;
		qp->comp.rnr_retry = attr->rnr_retry;
		pr_debug("set rnr retry count = %d\n", attr->rnr_retry);
	}

	if (mask & IB_QP_RQ_PSN) {
		qp->attr.rq_psn = (attr->rq_psn & BTH_PSN_MASK);
		qp->resp.psn = qp->attr.rq_psn;
		pr_debug("set resp psn = 0x%x\n", qp->resp.psn);
	}

	if (mask & IB_QP_MIN_RNR_TIMER) {
		qp->attr.min_rnr_timer = attr->min_rnr_timer;
		pr_debug("set min rnr timer = 0x%x\n",
			 attr->min_rnr_timer);
	}

	if (mask & IB_QP_SQ_PSN) {
		qp->attr.sq_psn = (attr->sq_psn & BTH_PSN_MASK);
		qp->req.psn = qp->attr.sq_psn;
		qp->comp.psn = qp->attr.sq_psn;
		pr_debug("set req psn = 0x%x\n", qp->req.psn);
	}

	if (mask & IB_QP_MAX_DEST_RD_ATOMIC) {
		qp->attr.max_dest_rd_atomic =
			__roundup_pow_of_two(attr->max_dest_rd_atomic);
	}

	if (mask & IB_QP_PATH_MIG_STATE)
		qp->attr.path_mig_state = attr->path_mig_state;

	if (mask & IB_QP_DEST_QPN)
		qp->attr.dest_qp_num = attr->dest_qp_num;

	if (mask & IB_QP_STATE) {
		qp->attr.qp_state = attr->qp_state;

		switch (attr->qp_state) {
		case IB_QPS_RESET:
			pr_debug("qp state -> RESET\n");
			rvt_qp_reset(qp);
			break;

		case IB_QPS_INIT:
			pr_debug("qp state -> INIT\n");
			qp->req.state = QP_STATE_INIT;
			qp->resp.state = QP_STATE_INIT;
			break;

		case IB_QPS_RTR:
			pr_debug("qp state -> RTR\n");
			qp->resp.state = QP_STATE_READY;
			break;

		case IB_QPS_RTS:
			pr_debug("qp state -> RTS\n");
			qp->req.state = QP_STATE_READY;
			break;

		case IB_QPS_SQD:
			pr_debug("qp state -> SQD\n");
			rvt_qp_drain(qp);
			break;

		case IB_QPS_SQE:
			pr_warn("qp state -> SQE !!?\n");
			/* Not possible from modify_qp. */
			break;

		case IB_QPS_ERR:
			pr_debug("qp state -> ERR\n");
			rvt_qp_error(qp);
			break;
		}
	}

	return 0;
}

/* called by the query qp verb */
int rvt_qp_to_attr(struct rvt_qp *qp, struct ib_qp_attr *attr, int mask)
{
	struct rvt_dev *rvt = to_rdev(qp->ibqp.device);

	*attr = qp->attr;

	attr->rq_psn				= qp->resp.psn;
	attr->sq_psn				= qp->req.psn;

	attr->cap.max_send_wr			= qp->sq.max_wr;
	attr->cap.max_send_sge			= qp->sq.max_sge;
	attr->cap.max_inline_data		= qp->sq.max_inline;

	if (!qp->srq) {
		attr->cap.max_recv_wr		= qp->rq.max_wr;
		attr->cap.max_recv_sge		= qp->rq.max_sge;
	}

	rvt_av_to_attr(rvt, &qp->pri_av, &attr->ah_attr);
	rvt_av_to_attr(rvt, &qp->alt_av, &attr->alt_ah_attr);

	if (qp->req.state == QP_STATE_DRAIN) {
		attr->sq_draining = 1;
		/* applications that get this state
		 * typically spin on it. yield the
		 * processor
		 */
		cond_resched();
	} else {
		attr->sq_draining = 0;
	}

	pr_debug("attr->sq_draining = %d\n", attr->sq_draining);

	return 0;
}

/* called by the destroy qp verb */
void rvt_qp_destroy(struct rvt_qp *qp)
{
	qp->valid = 0;
	qp->qp_timeout_jiffies = 0;
	rvt_cleanup_task(&qp->resp.task);

	del_timer_sync(&qp->retrans_timer);
	del_timer_sync(&qp->rnr_nak_timer);

	rvt_cleanup_task(&qp->req.task);
	if (qp_type(qp) == IB_QPT_RC)
		rvt_cleanup_task(&qp->comp.task);

	/* flush out any receive wr's or pending requests */
	__rvt_do_task(&qp->req.task);
	if (qp->sq.queue) {
		__rvt_do_task(&qp->comp.task);
		__rvt_do_task(&qp->req.task);
	}
}

/* called when the last reference to the qp is dropped */
void rvt_qp_cleanup(void *arg)
{
	struct rvt_qp *qp = arg;
	struct rvt_dev *rdev;

	rdev = to_rdev(qp->ibqp.device);
	rvt_drop_all_mcast_groups(qp);

	if (qp->sq.queue)
		rvt_queue_cleanup(qp->sq.queue);

	if (qp->srq)
		rvt_drop_ref(qp->srq);

	if (qp->rq.queue)
		rvt_queue_cleanup(qp->rq.queue);

	if (qp->scq)
		rvt_drop_ref(qp->scq);
	if (qp->rcq)
		rvt_drop_ref(qp->rcq);
	if (qp->pd)
		rvt_drop_ref(qp->pd);

	if (qp->resp.mr) {
		rvt_drop_ref(qp->resp.mr);
		qp->resp.mr = NULL;
	}

	free_rd_atomic_resources(qp);

	if (rdev)
		rdev->ifc_ops->destroy_flow(rdev, qp->flow);
}
