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
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
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

#include "rvt_loc.h"
#include "rvt_queue.h"

void rvt_cleanup_ports(struct rvt_dev *rvt);

static int rvt_query_device(struct ib_device *dev,
			    struct ib_device_attr *attr,
			    struct ib_udata *uhw)
{
	struct rvt_dev *rvt = to_rdev(dev);

	if (uhw->inlen || uhw->outlen)
		return -EINVAL;

	*attr = rvt->attr;
	return 0;
}

static void rvt_eth_speed_to_ib_speed(int speed, u8 *active_speed,
		u8 *active_width)
{
	if (speed <= 1000) {
		*active_width = IB_WIDTH_1X;
		*active_speed = IB_SPEED_SDR;
	} else if (speed <= 10000) {
		*active_width = IB_WIDTH_1X;
		*active_speed = IB_SPEED_FDR10;
	} else if (speed <= 20000) {
		*active_width = IB_WIDTH_4X;
		*active_speed = IB_SPEED_DDR;
	} else if (speed <= 30000) {
		*active_width = IB_WIDTH_4X;
		*active_speed = IB_SPEED_QDR;
	} else if (speed <= 40000) {
		*active_width = IB_WIDTH_4X;
		*active_speed = IB_SPEED_FDR10;
	} else {
		*active_width = IB_WIDTH_4X;
		*active_speed = IB_SPEED_EDR;
	}
}

static int rvt_query_port(struct ib_device *dev,
			  u8 port_num, struct ib_port_attr *attr)
{
	struct rvt_dev *rvt = to_rdev(dev);
	struct rvt_port *port;

	if (unlikely(port_num < 1 || port_num > rvt->num_ports)) {
		pr_warn("invalid port_number %d\n", port_num);
		goto err1;
	}

	port = &rvt->port[port_num - 1];

	*attr = port->attr;
	return 0;

err1:
	return -EINVAL;
}

static int rvt_query_gid(struct ib_device *device,
			 u8 port_num, int index, union ib_gid *gid)
{
	int ret;

	if (index > RVT_PORT_GID_TBL_LEN)
		return -EINVAL;

	ret = ib_get_cached_gid(device, port_num, index, gid, NULL);
	if (ret == -EAGAIN) {
		memcpy(gid, &zgid, sizeof(*gid));
		return 0;
	}

	return ret;
}

static int rvt_add_gid(struct ib_device *device, u8 port_num, unsigned int
		       index, const union ib_gid *gid,
		       const struct ib_gid_attr *attr, void **context)
{
	return 0;
}

static int rvt_del_gid(struct ib_device *device, u8 port_num, unsigned int
		       index, void **context)
{
	return 0;
}

static struct net_device *rvt_get_netdev(struct ib_device *device,
					 u8 port_num)
{
	struct rvt_dev *rdev = to_rdev(device);

	if (rdev->ifc_ops->get_netdev)
		return rdev->ifc_ops->get_netdev(rdev, port_num);

	return NULL;
}

static int rvt_query_pkey(struct ib_device *device,
			  u8 port_num, u16 index, u16 *pkey)
{
	struct rvt_dev *rvt = to_rdev(device);
	struct rvt_port *port;

	if (unlikely(port_num < 1 || port_num > rvt->num_ports)) {
		dev_warn(device->dma_device, "invalid port_num = %d\n",
			 port_num);
		goto err1;
	}

	port = &rvt->port[port_num - 1];

	if (unlikely(index >= port->attr.pkey_tbl_len)) {
		dev_warn(device->dma_device, "invalid index = %d\n",
			 index);
		goto err1;
	}

	*pkey = port->pkey_tbl[index];
	return 0;

err1:
	return -EINVAL;
}

static int rvt_modify_device(struct ib_device *dev,
			     int mask, struct ib_device_modify *attr)
{
	struct rvt_dev *rvt = to_rdev(dev);

	if (mask & IB_DEVICE_MODIFY_SYS_IMAGE_GUID)
		rvt->attr.sys_image_guid = cpu_to_be64(attr->sys_image_guid);

	if (mask & IB_DEVICE_MODIFY_NODE_DESC) {
		memcpy(rvt->ib_dev.node_desc,
		       attr->node_desc, sizeof(rvt->ib_dev.node_desc));
	}

	return 0;
}

static int rvt_modify_port(struct ib_device *dev,
			   u8 port_num, int mask, struct ib_port_modify *attr)
{
	struct rvt_dev *rvt = to_rdev(dev);
	struct rvt_port *port;

	if (unlikely(port_num < 1 || port_num > rvt->num_ports)) {
		pr_warn("invalid port_num = %d\n", port_num);
		goto err1;
	}

	port = &rvt->port[port_num - 1];

	port->attr.port_cap_flags |= attr->set_port_cap_mask;
	port->attr.port_cap_flags &= ~attr->clr_port_cap_mask;

	if (mask & IB_PORT_RESET_QKEY_CNTR)
		port->attr.qkey_viol_cntr = 0;

	return 0;

err1:
	return -EINVAL;
}

static enum rdma_link_layer rvt_get_link_layer(struct ib_device *dev,
					       u8 port_num)
{
	struct rvt_dev *rvt = to_rdev(dev);

	return rvt->ifc_ops->link_layer(rvt, port_num);
}

static struct ib_ucontext *rvt_alloc_ucontext(struct ib_device *dev,
					      struct ib_udata *udata)
{
	struct rvt_dev *rvt = to_rdev(dev);
	struct rvt_ucontext *uc;

	uc = rvt_alloc(&rvt->uc_pool);
	return uc ? &uc->ibuc : ERR_PTR(-ENOMEM);
}

static int rvt_dealloc_ucontext(struct ib_ucontext *ibuc)
{
	struct rvt_ucontext *uc = to_ruc(ibuc);

	rvt_drop_ref(uc);
	return 0;
}

static int rvt_port_immutable(struct ib_device *dev, u8 port_num,
			      struct ib_port_immutable *immutable)
{
	int err;
	struct ib_port_attr attr;

	err = rvt_query_port(dev, port_num, &attr);
	if (err)
		return err;

	immutable->pkey_tbl_len = attr.pkey_tbl_len;
	immutable->gid_tbl_len = attr.gid_tbl_len;
	immutable->core_cap_flags = RDMA_CORE_PORT_IBA_ROCE_UDP_ENCAP;
	immutable->max_mad_size = IB_MGMT_MAD_SIZE;

	return 0;
}

static struct ib_pd *rvt_alloc_pd(struct ib_device *dev,
				  struct ib_ucontext *context,
				  struct ib_udata *udata)
{
	struct rvt_dev *rvt = to_rdev(dev);
	struct rvt_pd *pd;

	pd = rvt_alloc(&rvt->pd_pool);
	return pd ? &pd->ibpd : ERR_PTR(-ENOMEM);
}

static int rvt_dealloc_pd(struct ib_pd *ibpd)
{
	struct rvt_pd *pd = to_rpd(ibpd);

	rvt_drop_ref(pd);
	return 0;
}

static int rvt_init_av(struct rvt_dev *rvt, struct ib_ah_attr *attr,
		       union ib_gid *sgid, struct ib_gid_attr *sgid_attr,
		       struct rvt_av *av)
{
	int err;

	err = ib_get_cached_gid(&rvt->ib_dev, attr->port_num,
				attr->grh.sgid_index, sgid,
				sgid_attr);
	if (err) {
		pr_err("Failed to query sgid. err = %d\n", err);
		return err;
	}

	err = rvt_av_from_attr(rvt, attr->port_num, av, attr);
	if (err)
		return err;

	err = rvt_av_fill_ip_info(rvt, av, attr, sgid_attr, sgid);
	if (err)
		return err;

	return 0;
}

static struct ib_ah *rvt_create_ah(struct ib_pd *ibpd, struct ib_ah_attr *attr)
{
	int err;
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_ah *ah;
	union ib_gid sgid;
	struct ib_gid_attr sgid_attr;

	err = rvt_av_chk_attr(rvt, attr);
	if (err)
		goto err1;

	ah = rvt_alloc(&rvt->ah_pool);
	if (!ah) {
		err = -ENOMEM;
		goto err1;
	}

	rvt_add_ref(pd);
	ah->pd = pd;

	err = rvt_init_av(rvt, attr, &sgid, &sgid_attr, &ah->av);
	if (err)
		goto err2;

	return &ah->ibah;

err2:
	rvt_drop_ref(pd);
	rvt_drop_ref(ah);
err1:
	return ERR_PTR(err);
}

static int rvt_modify_ah(struct ib_ah *ibah, struct ib_ah_attr *attr)
{
	int err;
	struct rvt_dev *rvt = to_rdev(ibah->device);
	struct rvt_ah *ah = to_rah(ibah);
	union ib_gid sgid;
	struct ib_gid_attr sgid_attr;

	err = rvt_av_chk_attr(rvt, attr);
	if (err)
		return err;

	err = rvt_init_av(rvt, attr, &sgid, &sgid_attr, &ah->av);
	if (err)
		return err;

	return 0;
}

static int rvt_query_ah(struct ib_ah *ibah, struct ib_ah_attr *attr)
{
	struct rvt_dev *rvt = to_rdev(ibah->device);
	struct rvt_ah *ah = to_rah(ibah);

	rvt_av_to_attr(rvt, &ah->av, attr);
	return 0;
}

static int rvt_destroy_ah(struct ib_ah *ibah)
{
	struct rvt_ah *ah = to_rah(ibah);

	rvt_drop_ref(ah->pd);
	rvt_drop_ref(ah);
	return 0;
}

static int post_one_recv(struct rvt_rq *rq, struct ib_recv_wr *ibwr)
{
	int err;
	int i;
	u32 length;
	struct rvt_recv_wqe *recv_wqe;
	int num_sge = ibwr->num_sge;

	if (unlikely(queue_full(rq->queue))) {
		err = -ENOMEM;
		goto err1;
	}

	if (unlikely(num_sge > rq->max_sge)) {
		err = -EINVAL;
		goto err1;
	}

	length = 0;
	for (i = 0; i < num_sge; i++)
		length += ibwr->sg_list[i].length;

	recv_wqe = producer_addr(rq->queue);
	recv_wqe->wr_id = ibwr->wr_id;
	recv_wqe->num_sge = num_sge;

	memcpy(recv_wqe->dma.sge, ibwr->sg_list,
	       num_sge * sizeof(struct ib_sge));

	recv_wqe->dma.length		= length;
	recv_wqe->dma.resid		= length;
	recv_wqe->dma.num_sge		= num_sge;
	recv_wqe->dma.cur_sge		= 0;
	recv_wqe->dma.sge_offset	= 0;

	/* make sure all changes to the work queue are written before we
	 * update the producer pointer
	 */
	smp_wmb();

	advance_producer(rq->queue);
	return 0;

err1:
	return err;
}

static struct ib_srq *rvt_create_srq(struct ib_pd *ibpd,
				     struct ib_srq_init_attr *init,
				     struct ib_udata *udata)
{
	int err;
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_srq *srq;
	struct ib_ucontext *context = udata ? ibpd->uobject->context : NULL;

	err = rvt_srq_chk_attr(rvt, NULL, &init->attr, IB_SRQ_INIT_MASK);
	if (err)
		goto err1;

	srq = rvt_alloc(&rvt->srq_pool);
	if (!srq) {
		err = -ENOMEM;
		goto err1;
	}

	rvt_add_index(srq);
	rvt_add_ref(pd);
	srq->pd = pd;

	err = rvt_srq_from_init(rvt, srq, init, context, udata);
	if (err)
		goto err2;

	return &srq->ibsrq;

err2:
	rvt_drop_ref(pd);
	rvt_drop_index(srq);
	rvt_drop_ref(srq);
err1:
	return ERR_PTR(err);
}

static int rvt_modify_srq(struct ib_srq *ibsrq, struct ib_srq_attr *attr,
			  enum ib_srq_attr_mask mask,
			  struct ib_udata *udata)
{
	int err;
	struct rvt_srq *srq = to_rsrq(ibsrq);
	struct rvt_dev *rvt = to_rdev(ibsrq->device);

	err = rvt_srq_chk_attr(rvt, srq, attr, mask);
	if (err)
		goto err1;

	err = rvt_srq_from_attr(rvt, srq, attr, mask, udata);
	if (err)
		goto err1;

	return 0;

err1:
	return err;
}

static int rvt_query_srq(struct ib_srq *ibsrq, struct ib_srq_attr *attr)
{
	struct rvt_srq *srq = to_rsrq(ibsrq);

	if (srq->error)
		return -EINVAL;

	attr->max_wr = srq->rq.queue->buf->index_mask;
	attr->max_sge = srq->rq.max_sge;
	attr->srq_limit = srq->limit;
	return 0;
}

static int rvt_destroy_srq(struct ib_srq *ibsrq)
{
	struct rvt_srq *srq = to_rsrq(ibsrq);

	if (srq->cq)
		rvt_drop_ref(srq->cq);

	if(srq->rq.queue)
		rvt_queue_cleanup(srq->rq.queue);

	rvt_drop_ref(srq->pd);
	rvt_drop_index(srq);
	rvt_drop_ref(srq);

	return 0;
}

static int rvt_post_srq_recv(struct ib_srq *ibsrq, struct ib_recv_wr *wr,
			     struct ib_recv_wr **bad_wr)
{
	int err = 0;
	unsigned long flags;
	struct rvt_srq *srq = to_rsrq(ibsrq);

	spin_lock_irqsave(&srq->rq.producer_lock, flags);

	while (wr) {
		err = post_one_recv(&srq->rq, wr);
		if (unlikely(err))
			break;
		wr = wr->next;
	}

	spin_unlock_irqrestore(&srq->rq.producer_lock, flags);

	if (err)
		*bad_wr = wr;

	return err;
}

static struct ib_qp *rvt_create_qp(struct ib_pd *ibpd,
				   struct ib_qp_init_attr *init,
				   struct ib_udata *udata)
{
	int err;
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_qp *qp;

	err = rvt_qp_chk_init(rvt, init);
	if (err)
		goto err1;

	qp = rvt_alloc(&rvt->qp_pool);
	if (!qp) {
		err = -ENOMEM;
		goto err1;
	}

	rvt_add_index(qp);

	if (udata)
		qp->is_user = 1;

	err = rvt_qp_from_init(rvt, qp, pd, init, udata, ibpd);
	if (err)
		goto err2;

	return &qp->ibqp;

err2:
	rvt_drop_index(qp);
	rvt_drop_ref(qp);
err1:
	return ERR_PTR(err);
}

static int rvt_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			 int mask, struct ib_udata *udata)
{
	int err;
	struct rvt_dev *rvt = to_rdev(ibqp->device);
	struct rvt_qp *qp = to_rqp(ibqp);

	err = rvt_qp_chk_attr(rvt, qp, attr, mask);
	if (err)
		goto err1;

	err = rvt_qp_from_attr(qp, attr, mask, udata);
	if (err)
		goto err1;

	return 0;

err1:
	return err;
}

static int rvt_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
			int mask, struct ib_qp_init_attr *init)
{
	struct rvt_qp *qp = to_rqp(ibqp);

	rvt_qp_to_init(qp, init);
	rvt_qp_to_attr(qp, attr, mask);

	return 0;
}

static int rvt_destroy_qp(struct ib_qp *ibqp)
{
	struct rvt_qp *qp = to_rqp(ibqp);

	rvt_qp_destroy(qp);
	rvt_drop_index(qp);
	rvt_drop_ref(qp);
	return 0;
}

static int validate_send_wr(struct rvt_qp *qp, struct ib_send_wr *ibwr,
			    unsigned int mask, unsigned int length)
{
	int num_sge = ibwr->num_sge;
	struct rvt_sq *sq = &qp->sq;

	if (unlikely(num_sge > sq->max_sge))
		goto err1;

	if (unlikely(mask & WR_ATOMIC_MASK)) {
		if (length < 8)
			goto err1;

		if (atomic_wr(ibwr)->remote_addr & 0x7)
			goto err1;
	}

	if (unlikely((ibwr->send_flags & IB_SEND_INLINE) &&
		     (length > sq->max_inline)))
		goto err1;

	return 0;

err1:
	return -EINVAL;
}

static void init_send_wr(struct rvt_qp *qp, struct rvt_send_wr *wr,
			 struct ib_send_wr *ibwr)
{
	wr->wr_id = ibwr->wr_id;
	wr->num_sge = ibwr->num_sge;
	wr->opcode = ibwr->opcode;
	wr->send_flags = ibwr->send_flags;

	if (qp_type(qp) == IB_QPT_UD ||
	    qp_type(qp) == IB_QPT_SMI ||
	    qp_type(qp) == IB_QPT_GSI) {
		wr->wr.ud.remote_qpn = ud_wr(ibwr)->remote_qpn;
		wr->wr.ud.remote_qkey = ud_wr(ibwr)->remote_qkey;
		if (qp_type(qp) == IB_QPT_GSI)
			wr->wr.ud.pkey_index = ud_wr(ibwr)->pkey_index;
		if (wr->opcode == IB_WR_SEND_WITH_IMM)
			wr->ex.imm_data = ibwr->ex.imm_data;
	} else {
		switch (wr->opcode) {
		case IB_WR_RDMA_WRITE_WITH_IMM:
			wr->ex.imm_data = ibwr->ex.imm_data;
		case IB_WR_RDMA_READ:
		case IB_WR_RDMA_WRITE:
			wr->wr.rdma.remote_addr = rdma_wr(ibwr)->remote_addr;
			wr->wr.rdma.rkey	= rdma_wr(ibwr)->rkey;
			break;
		case IB_WR_SEND_WITH_IMM:
			wr->ex.imm_data = ibwr->ex.imm_data;
			break;
		case IB_WR_SEND_WITH_INV:
			wr->ex.invalidate_rkey = ibwr->ex.invalidate_rkey;
			break;
		case IB_WR_ATOMIC_CMP_AND_SWP:
		case IB_WR_ATOMIC_FETCH_AND_ADD:
			wr->wr.atomic.remote_addr =
				atomic_wr(ibwr)->remote_addr;
			wr->wr.atomic.compare_add =
				atomic_wr(ibwr)->compare_add;
			wr->wr.atomic.swap = atomic_wr(ibwr)->swap;
			wr->wr.atomic.rkey = atomic_wr(ibwr)->rkey;
			break;
		default:
			break;
		}
	}
}

static int init_send_wqe(struct rvt_qp *qp, struct ib_send_wr *ibwr,
			 unsigned int mask, unsigned int length,
			 struct rvt_send_wqe *wqe)
{
	int num_sge = ibwr->num_sge;
	struct ib_sge *sge;
	int i;
	u8 *p;

	init_send_wr(qp, &wqe->wr, ibwr);

	if (qp_type(qp) == IB_QPT_UD ||
	    qp_type(qp) == IB_QPT_SMI ||
	    qp_type(qp) == IB_QPT_GSI)
		memcpy(&wqe->av, &to_rah(ud_wr(ibwr)->ah)->av, sizeof(wqe->av));

	if (unlikely(ibwr->send_flags & IB_SEND_INLINE)) {
		p = wqe->dma.inline_data;

		sge = ibwr->sg_list;
		for (i = 0; i < num_sge; i++, sge++) {
			if (qp->is_user && copy_from_user(p, (__user void *)
					    (uintptr_t)sge->addr, sge->length))
				return -EFAULT;

			else if (!qp->is_user)
				memcpy(p, (void *)(uintptr_t)sge->addr,
				       sge->length);

			p += sge->length;
		}
	} else
		memcpy(wqe->dma.sge, ibwr->sg_list,
		       num_sge * sizeof(struct ib_sge));

	wqe->iova		= (mask & WR_ATOMIC_MASK) ?
					atomic_wr(ibwr)->remote_addr :
					atomic_wr(ibwr)->remote_addr;
	wqe->mask		= mask;
	wqe->dma.length		= length;
	wqe->dma.resid		= length;
	wqe->dma.num_sge	= num_sge;
	wqe->dma.cur_sge	= 0;
	wqe->dma.sge_offset	= 0;
	wqe->state		= wqe_state_posted;
	wqe->ssn		= atomic_add_return(1, &qp->ssn);

	return 0;
}

static int post_one_send(struct rvt_qp *qp, struct ib_send_wr *ibwr,
			 unsigned mask, u32 length)
{
	int err;
	struct rvt_sq *sq = &qp->sq;
	struct rvt_send_wqe *send_wqe;
	unsigned long flags;

	err = validate_send_wr(qp, ibwr, mask, length);
	if (err)
		return err;

	spin_lock_irqsave(&qp->sq.sq_lock, flags);

	if (unlikely(queue_full(sq->queue))) {
		err = -ENOMEM;
		goto err1;
	}

	send_wqe = producer_addr(sq->queue);

	err = init_send_wqe(qp, ibwr, mask, length, send_wqe);
	if (unlikely(err))
		goto err1;

	/* make sure all changes to the work queue are
	   written before we update the producer pointer */
	smp_wmb();

	advance_producer(sq->queue);
	spin_unlock_irqrestore(&qp->sq.sq_lock, flags);

	return 0;

err1:
	spin_unlock_irqrestore(&qp->sq.sq_lock, flags);
	return err;
}

static int rvt_post_send(struct ib_qp *ibqp, struct ib_send_wr *wr,
			 struct ib_send_wr **bad_wr)
{
	int err = 0;
	struct rvt_qp *qp = to_rqp(ibqp);
	unsigned int mask;
	unsigned int length = 0;
	int i;
	int must_sched;

	if (unlikely(!qp->valid)) {
		*bad_wr = wr;
		return -EINVAL;
	}

	if (unlikely(qp->req.state < QP_STATE_READY)) {
		*bad_wr = wr;
		return -EINVAL;
	}

	while (wr) {
		mask = wr_opcode_mask(wr->opcode, qp);
		if (unlikely(!mask)) {
			err = -EINVAL;
			*bad_wr = wr;
			break;
		}

		if (unlikely((wr->send_flags & IB_SEND_INLINE) &&
			     !(mask & WR_INLINE_MASK))) {
			err = -EINVAL;
			*bad_wr = wr;
			break;
		}

		length = 0;
		for (i = 0; i < wr->num_sge; i++)
			length += wr->sg_list[i].length;

		err = post_one_send(qp, wr, mask, length);

		if (err) {
			*bad_wr = wr;
			break;
		}
		wr = wr->next;
	}

	/*
	 * Must sched in case of GSI QP because ib_send_mad() hold irq lock,
	 * and the requester call ip_local_out_sk() that takes spin_lock_bh.
	 */
	must_sched = (qp_type(qp) == IB_QPT_GSI) ||
			(queue_count(qp->sq.queue) > 1);

	rvt_run_task(&qp->req.task, must_sched);

	return err;
}

static int rvt_post_recv(struct ib_qp *ibqp, struct ib_recv_wr *wr,
			 struct ib_recv_wr **bad_wr)
{
	int err = 0;
	struct rvt_qp *qp = to_rqp(ibqp);
	struct rvt_rq *rq = &qp->rq;
	unsigned long flags;

	if (unlikely((qp_state(qp) < IB_QPS_INIT) || !qp->valid)) {
		*bad_wr = wr;
		err = -EINVAL;
		goto err1;
	}

	if (unlikely(qp->srq)) {
		*bad_wr = wr;
		err = -EINVAL;
		goto err1;
	}

	spin_lock_irqsave(&rq->producer_lock, flags);

	while (wr) {
		err = post_one_recv(rq, wr);
		if (unlikely(err)) {
			*bad_wr = wr;
			break;
		}
		wr = wr->next;
	}

	spin_unlock_irqrestore(&rq->producer_lock, flags);

err1:
	return err;
}

static struct ib_cq *rvt_create_cq(struct ib_device *dev,
				   const struct ib_cq_init_attr *attr,
				   struct ib_ucontext *context,
				   struct ib_udata *udata)
{
	int err;
	struct rvt_dev *rvt = to_rdev(dev);
	struct rvt_cq *cq;

	if (attr->flags)
		return ERR_PTR(-EINVAL);

	err = rvt_cq_chk_attr(rvt, NULL, attr->cqe, attr->comp_vector, udata);
	if (err)
		goto err1;

	cq = rvt_alloc(&rvt->cq_pool);
	if (!cq) {
		err = -ENOMEM;
		goto err1;
	}

	err = rvt_cq_from_init(rvt, cq, attr->cqe, attr->comp_vector,
			       context, udata);
	if (err)
		goto err2;

	return &cq->ibcq;

err2:
	rvt_drop_ref(cq);
err1:
	return ERR_PTR(err);
}

static int rvt_destroy_cq(struct ib_cq *ibcq)
{
	struct rvt_cq *cq = to_rcq(ibcq);

	rvt_drop_ref(cq);
	return 0;
}

static int rvt_resize_cq(struct ib_cq *ibcq, int cqe, struct ib_udata *udata)
{
	int err;
	struct rvt_cq *cq = to_rcq(ibcq);
	struct rvt_dev *rvt = to_rdev(ibcq->device);

	err = rvt_cq_chk_attr(rvt, cq, cqe, 0, udata);
	if (err)
		goto err1;

	err = rvt_cq_resize_queue(cq, cqe, udata);
	if (err)
		goto err1;

	return 0;

err1:
	return err;
}

static int rvt_poll_cq(struct ib_cq *ibcq, int num_entries, struct ib_wc *wc)
{
	int i;
	struct rvt_cq *cq = to_rcq(ibcq);
	struct rvt_cqe *cqe;

	for (i = 0; i < num_entries; i++) {
		cqe = queue_head(cq->queue);
		if (!cqe)
			break;

		memcpy(wc++, &cqe->ibwc, sizeof(*wc));
		advance_consumer(cq->queue);
	}

	return i;
}

static int rvt_peek_cq(struct ib_cq *ibcq, int wc_cnt)
{
	struct rvt_cq *cq = to_rcq(ibcq);
	int count = queue_count(cq->queue);

	return (count > wc_cnt) ? wc_cnt : count;
}

static int rvt_req_notify_cq(struct ib_cq *ibcq, enum ib_cq_notify_flags flags)
{
	struct rvt_cq *cq = to_rcq(ibcq);

	if (cq->notify != IB_CQ_NEXT_COMP)
		cq->notify = flags & IB_CQ_SOLICITED_MASK;

	return 0;
}

static struct ib_mr *rvt_get_dma_mr(struct ib_pd *ibpd, int access)
{
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_mem *mr;
	int err;

	mr = rvt_alloc(&rvt->mr_pool);
	if (!mr) {
		err = -ENOMEM;
		goto err1;
	}

	rvt_add_index(mr);

	rvt_add_ref(pd);

	err = rvt_mem_init_dma(rvt, pd, access, mr);
	if (err)
		goto err2;

	return &mr->ibmr;

err2:
	rvt_drop_ref(pd);
	rvt_drop_index(mr);
	rvt_drop_ref(mr);
err1:
	return ERR_PTR(err);
}

static struct ib_mr *rvt_reg_phys_mr(struct ib_pd *ibpd,
				     struct rvt_phys_buf *phys_buf_array,
				     int num_phys_buf,
				     int access, u64 *iova_start)
{
	int err;
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_mem *mr;
	u64 iova = *iova_start;

	mr = rvt_alloc(&rvt->mr_pool);
	if (!mr) {
		err = -ENOMEM;
		goto err1;
	}

	rvt_add_index(mr);

	rvt_add_ref(pd);

	err = rvt_mem_init_phys(rvt, pd, access, iova,
				phys_buf_array, num_phys_buf, mr);
	if (err)
		goto err2;

	return &mr->ibmr;

err2:
	rvt_drop_ref(pd);
	rvt_drop_index(mr);
	rvt_drop_ref(mr);
err1:
	return ERR_PTR(err);
}

static struct ib_mr *rvt_reg_user_mr(struct ib_pd *ibpd,
				     u64 start,
				     u64 length,
				     u64 iova,
				     int access, struct ib_udata *udata)
{
	int err;
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_mem *mr;

	mr = rvt_alloc(&rvt->mr_pool);
	if (!mr) {
		err = -ENOMEM;
		goto err2;
	}

	rvt_add_index(mr);

	rvt_add_ref(pd);

	err = rvt_mem_init_user(rvt, pd, start, length, iova,
				access, udata, mr);
	if (err)
		goto err3;

	return &mr->ibmr;

err3:
	rvt_drop_ref(pd);
	rvt_drop_index(mr);
	rvt_drop_ref(mr);
err2:
	return ERR_PTR(err);
}

static int rvt_dereg_mr(struct ib_mr *ibmr)
{
	struct rvt_mem *mr = to_rmr(ibmr);

	mr->state = RVT_MEM_STATE_ZOMBIE;
	rvt_drop_ref(mr->pd);
	rvt_drop_index(mr);
	rvt_drop_ref(mr);
	return 0;
}

static struct ib_mr *rvt_alloc_mr(struct ib_pd *ibpd,
				  enum ib_mr_type mr_type,
				  u32 max_num_sg)
{
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_mem *mr;
	int err;

	if (mr_type != IB_MR_TYPE_MEM_REG)
		return ERR_PTR(-EINVAL);

	mr = rvt_alloc(&rvt->mr_pool);
	if (!mr) {
		err = -ENOMEM;
		goto err1;
	}

	rvt_add_index(mr);

	rvt_add_ref(pd);

	err = rvt_mem_init_fast(rvt, pd, max_num_sg, mr);
	if (err)
		goto err2;

	return &mr->ibmr;

err2:
	rvt_drop_ref(pd);
	rvt_drop_index(mr);
	rvt_drop_ref(mr);
err1:
	return ERR_PTR(err);
}

static struct ib_mw *rvt_alloc_mw(struct ib_pd *ibpd, enum ib_mw_type type)
{
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_mem *mw;
	int err;

	if (type != IB_MW_TYPE_1)
		return ERR_PTR(-EINVAL);

	mw = rvt_alloc(&rvt->mw_pool);
	if (!mw) {
		err = -ENOMEM;
		goto err1;
	}

	rvt_add_index(mw);

	rvt_add_ref(pd);

	err = rvt_mem_init_mw(rvt, pd, mw);
	if (err)
		goto err2;

	return &mw->ibmw;

err2:
	rvt_drop_ref(pd);
	rvt_drop_index(mw);
	rvt_drop_ref(mw);
err1:
	return ERR_PTR(err);
}

static int rvt_dealloc_mw(struct ib_mw *ibmw)
{
	struct rvt_mem *mw = to_rmw(ibmw);

	mw->state = RVT_MEM_STATE_ZOMBIE;
	rvt_drop_ref(mw->pd);
	rvt_drop_index(mw);
	rvt_drop_ref(mw);
	return 0;
}

static struct ib_fmr *rvt_alloc_fmr(struct ib_pd *ibpd,
				    int access, struct ib_fmr_attr *attr)
{
	struct rvt_dev *rvt = to_rdev(ibpd->device);
	struct rvt_pd *pd = to_rpd(ibpd);
	struct rvt_mem *fmr;
	int err;

	fmr = rvt_alloc(&rvt->fmr_pool);
	if (!fmr) {
		err = -ENOMEM;
		goto err1;
	}

	rvt_add_index(fmr);

	rvt_add_ref(pd);

	err = rvt_mem_init_fmr(rvt, pd, access, attr, fmr);
	if (err)
		goto err2;

	return &fmr->ibfmr;

err2:
	rvt_drop_ref(pd);
	rvt_drop_index(fmr);
	rvt_drop_ref(fmr);
err1:
	return ERR_PTR(err);
}

static int rvt_map_phys_fmr(struct ib_fmr *ibfmr,
			    u64 *page_list, int list_length, u64 iova)
{
	struct rvt_mem *fmr = to_rfmr(ibfmr);
	struct rvt_dev *rvt = to_rdev(ibfmr->device);

	return rvt_mem_map_pages(rvt, fmr, page_list, list_length, iova);
}

static int rvt_unmap_fmr(struct list_head *fmr_list)
{
	struct rvt_mem *fmr;

	list_for_each_entry(fmr, fmr_list, ibfmr.list) {
		if (fmr->state != RVT_MEM_STATE_VALID)
			continue;

		fmr->va = 0;
		fmr->iova = 0;
		fmr->length = 0;
		fmr->num_buf = 0;
		fmr->state = RVT_MEM_STATE_FREE;
	}

	return 0;
}

static int rvt_dealloc_fmr(struct ib_fmr *ibfmr)
{
	struct rvt_mem *fmr = to_rfmr(ibfmr);

	fmr->state = RVT_MEM_STATE_ZOMBIE;
	rvt_drop_ref(fmr->pd);
	rvt_drop_index(fmr);
	rvt_drop_ref(fmr);
	return 0;
}

static int rvt_attach_mcast(struct ib_qp *ibqp, union ib_gid *mgid, u16 mlid)
{
	int err;
	struct rvt_dev *rvt = to_rdev(ibqp->device);
	struct rvt_qp *qp = to_rqp(ibqp);
	struct rvt_mc_grp *grp;

	/* takes a ref on grp if successful */
	err = rvt_mcast_get_grp(rvt, mgid, &grp);
	if (err)
		return err;

	err = rvt_mcast_add_grp_elem(rvt, qp, grp);

	rvt_drop_ref(grp);
	return err;
}

static int rvt_detach_mcast(struct ib_qp *ibqp, union ib_gid *mgid, u16 mlid)
{
	struct rvt_dev *rvt = to_rdev(ibqp->device);
	struct rvt_qp *qp = to_rqp(ibqp);

	return rvt_mcast_drop_grp_elem(rvt, qp, mgid);
}

static ssize_t rvt_show_parent(struct device *device,
			       struct device_attribute *attr, char *buf)
{
	struct rvt_dev *rvt = container_of(device, struct rvt_dev,
					   ib_dev.dev);
	char *name;

	name = rvt->ifc_ops->parent_name(rvt, 1);
	return snprintf(buf, 16, "%s\n", name);
}

static DEVICE_ATTR(parent, S_IRUGO, rvt_show_parent, NULL);

static struct device_attribute *rvt_dev_attributes[] = {
	&dev_attr_parent,
};

/* initialize port attributes */
static int rvt_init_port_param(struct rvt_dev *rdev, unsigned int port_num)
{
	struct rvt_port *port = &rdev->port[port_num - 1];

	port->attr.state		= RVT_PORT_STATE;
	port->attr.max_mtu		= RVT_PORT_MAX_MTU;
	port->attr.active_mtu		= RVT_PORT_ACTIVE_MTU;
	port->attr.gid_tbl_len		= RVT_PORT_GID_TBL_LEN;
	port->attr.port_cap_flags	= RVT_PORT_PORT_CAP_FLAGS;
	port->attr.max_msg_sz		= RVT_PORT_MAX_MSG_SZ;
	port->attr.bad_pkey_cntr	= RVT_PORT_BAD_PKEY_CNTR;
	port->attr.qkey_viol_cntr	= RVT_PORT_QKEY_VIOL_CNTR;
	port->attr.pkey_tbl_len		= RVT_PORT_PKEY_TBL_LEN;
	port->attr.lid			= RVT_PORT_LID;
	port->attr.sm_lid		= RVT_PORT_SM_LID;
	port->attr.lmc			= RVT_PORT_LMC;
	port->attr.max_vl_num		= RVT_PORT_MAX_VL_NUM;
	port->attr.sm_sl		= RVT_PORT_SM_SL;
	port->attr.subnet_timeout	= RVT_PORT_SUBNET_TIMEOUT;
	port->attr.init_type_reply	= RVT_PORT_INIT_TYPE_REPLY;
	port->attr.active_width		= RVT_PORT_ACTIVE_WIDTH;
	port->attr.active_speed		= RVT_PORT_ACTIVE_SPEED;
	port->attr.phys_state		= RVT_PORT_PHYS_STATE;
	port->mtu_cap			= ib_mtu_enum_to_int(RVT_PORT_ACTIVE_MTU);
	port->subnet_prefix		= cpu_to_be64(RVT_PORT_SUBNET_PREFIX);

	return 0;
}

/* initialize port state, note IB convention that HCA ports are always
 * numbered from 1
 */
static int rvt_init_ports(struct rvt_dev *rdev)
{
	int err;
	unsigned int port_num;
	struct rvt_port *port;

	rdev->port = kcalloc(rdev->num_ports, sizeof(struct rvt_port),
			    GFP_KERNEL);
	if (!rdev->port)
		return -ENOMEM;

	for (port_num = 1; port_num <= rdev->num_ports; port_num++) {
		port = &rdev->port[port_num - 1];

		rvt_init_port_param(rdev, port_num);

		if (!port->attr.pkey_tbl_len) {
			err = -EINVAL;
			goto err1;
		}

		port->pkey_tbl = kcalloc(port->attr.pkey_tbl_len,
					 sizeof(*port->pkey_tbl), GFP_KERNEL);
		if (!port->pkey_tbl) {
			err = -ENOMEM;
			goto err1;
		}

		port->pkey_tbl[0] = 0xffff;

		if (!port->attr.gid_tbl_len) {
			kfree(port->pkey_tbl);
			err = -EINVAL;
			goto err1;
		}

		port->port_guid = rdev->ifc_ops->port_guid(rdev, port_num);

		spin_lock_init(&port->port_lock);
	}

	return 0;

err1:
	while (--port_num >= 1) {
		port = &rdev->port[port_num - 1];
		kfree(port->pkey_tbl);
	}

	kfree(rdev->port);
	return err;
}

/* initialize rdev device parameters */
static int rvt_init_device_param(struct rvt_dev *rdev)
{
	rdev->max_inline_data			= RVT_MAX_INLINE_DATA;

	rdev->attr.fw_ver			= RVT_FW_VER;
	rdev->attr.max_mr_size			= RVT_MAX_MR_SIZE;
	rdev->attr.page_size_cap			= RVT_PAGE_SIZE_CAP;
	rdev->attr.vendor_id			= RVT_VENDOR_ID;
	rdev->attr.vendor_part_id		= RVT_VENDOR_PART_ID;
	rdev->attr.hw_ver			= RVT_HW_VER;
	rdev->attr.max_qp			= RVT_MAX_QP;
	rdev->attr.max_qp_wr			= RVT_MAX_QP_WR;
	rdev->attr.device_cap_flags		= RVT_DEVICE_CAP_FLAGS;
	rdev->attr.max_sge			= RVT_MAX_SGE;
	rdev->attr.max_sge_rd			= RVT_MAX_SGE_RD;
	rdev->attr.max_cq			= RVT_MAX_CQ;
	rdev->attr.max_cqe			= (1 << RVT_MAX_LOG_CQE) - 1;
	rdev->attr.max_mr			= RVT_MAX_MR;
	rdev->attr.max_pd			= RVT_MAX_PD;
	rdev->attr.max_qp_rd_atom		= RVT_MAX_QP_RD_ATOM;
	rdev->attr.max_ee_rd_atom		= RVT_MAX_EE_RD_ATOM;
	rdev->attr.max_res_rd_atom		= RVT_MAX_RES_RD_ATOM;
	rdev->attr.max_qp_init_rd_atom		= RVT_MAX_QP_INIT_RD_ATOM;
	rdev->attr.max_ee_init_rd_atom		= RVT_MAX_EE_INIT_RD_ATOM;
	rdev->attr.atomic_cap			= RVT_ATOMIC_CAP;
	rdev->attr.max_ee			= RVT_MAX_EE;
	rdev->attr.max_rdd			= RVT_MAX_RDD;
	rdev->attr.max_mw			= RVT_MAX_MW;
	rdev->attr.max_raw_ipv6_qp		= RVT_MAX_RAW_IPV6_QP;
	rdev->attr.max_raw_ethy_qp		= RVT_MAX_RAW_ETHY_QP;
	rdev->attr.max_mcast_grp			= RVT_MAX_MCAST_GRP;
	rdev->attr.max_mcast_qp_attach		= RVT_MAX_MCAST_QP_ATTACH;
	rdev->attr.max_total_mcast_qp_attach	= RVT_MAX_TOT_MCAST_QP_ATTACH;
	rdev->attr.max_ah			= RVT_MAX_AH;
	rdev->attr.max_fmr			= RVT_MAX_FMR;
	rdev->attr.max_map_per_fmr		= RVT_MAX_MAP_PER_FMR;
	rdev->attr.max_srq			= RVT_MAX_SRQ;
	rdev->attr.max_srq_wr			= RVT_MAX_SRQ_WR;
	rdev->attr.max_srq_sge			= RVT_MAX_SRQ_SGE;
	rdev->attr.max_fast_reg_page_list_len	= RVT_MAX_FMR_PAGE_LIST_LEN;
	rdev->attr.max_pkeys			= RVT_MAX_PKEYS;
	rdev->attr.local_ca_ack_delay		= RVT_LOCAL_CA_ACK_DELAY;

	rdev->max_ucontext			= RVT_MAX_UCONTEXT;

	return 0;
}

/* init pools of managed objects */
static int rvt_init_pools(struct rvt_dev *rdev)
{
	int err;

	err = rvt_pool_init(rdev, &rdev->uc_pool, RVT_TYPE_UC,
			    rdev->max_ucontext);
	if (err)
		goto err1;

	err = rvt_pool_init(rdev, &rdev->pd_pool, RVT_TYPE_PD,
			    rdev->attr.max_pd);
	if (err)
		goto err2;

	err = rvt_pool_init(rdev, &rdev->ah_pool, RVT_TYPE_AH,
			    rdev->attr.max_ah);
	if (err)
		goto err3;

	err = rvt_pool_init(rdev, &rdev->srq_pool, RVT_TYPE_SRQ,
			    rdev->attr.max_srq);
	if (err)
		goto err4;

	err = rvt_pool_init(rdev, &rdev->qp_pool, RVT_TYPE_QP,
			    rdev->attr.max_qp);
	if (err)
		goto err5;

	err = rvt_pool_init(rdev, &rdev->cq_pool, RVT_TYPE_CQ,
			    rdev->attr.max_cq);
	if (err)
		goto err6;

	err = rvt_pool_init(rdev, &rdev->mr_pool, RVT_TYPE_MR,
			    rdev->attr.max_mr);
	if (err)
		goto err7;

	err = rvt_pool_init(rdev, &rdev->fmr_pool, RVT_TYPE_FMR,
			    rdev->attr.max_fmr);
	if (err)
		goto err8;

	err = rvt_pool_init(rdev, &rdev->mw_pool, RVT_TYPE_MW,
			    rdev->attr.max_mw);
	if (err)
		goto err9;

	err = rvt_pool_init(rdev, &rdev->mc_grp_pool, RVT_TYPE_MC_GRP,
			    rdev->attr.max_mcast_grp);
	if (err)
		goto err10;

	err = rvt_pool_init(rdev, &rdev->mc_elem_pool, RVT_TYPE_MC_ELEM,
			    rdev->attr.max_total_mcast_qp_attach);
	if (err)
		goto err11;

	return 0;

err11:
	rvt_pool_cleanup(&rdev->mc_grp_pool);
err10:
	rvt_pool_cleanup(&rdev->mw_pool);
err9:
	rvt_pool_cleanup(&rdev->fmr_pool);
err8:
	rvt_pool_cleanup(&rdev->mr_pool);
err7:
	rvt_pool_cleanup(&rdev->cq_pool);
err6:
	rvt_pool_cleanup(&rdev->qp_pool);
err5:
	rvt_pool_cleanup(&rdev->srq_pool);
err4:
	rvt_pool_cleanup(&rdev->ah_pool);
err3:
	rvt_pool_cleanup(&rdev->pd_pool);
err2:
	rvt_pool_cleanup(&rdev->uc_pool);
err1:
	return err;
}

/* initialize rdev device state */
static int rvt_init(struct rvt_dev *rdev)
{
	int err;

	/* init default device parameters */
	rvt_init_device_param(rdev);

	err = rvt_init_ports(rdev);
	if (err)
		goto err1;

	err = rvt_init_pools(rdev);
	if (err)
		goto err2;

	/* init pending mmap list */
	spin_lock_init(&rdev->mmap_offset_lock);
	spin_lock_init(&rdev->pending_lock);
	INIT_LIST_HEAD(&rdev->pending_mmaps);

	mutex_init(&rdev->usdev_lock);

	return 0;

err2:
	rvt_cleanup_ports(rdev);
err1:
	return err;
}

struct rvt_dev* rvt_alloc_device(size_t size)
{
	struct rvt_dev *rdev;

	rdev = (struct rvt_dev *)ib_alloc_device(size);
	if (!rdev)
		return NULL;

	kref_init(&rdev->ref_cnt);
	
	return rdev;
}
EXPORT_SYMBOL_GPL(rvt_alloc_device);

int rvt_register_device(struct rvt_dev *rdev,
			struct rvt_ifc_ops *ops,
			unsigned int mtu)
{
	int err;
	int i;
	struct ib_device *dev;

	if (rdev->num_ports == 0)
		return -EINVAL;

	rdev->ifc_ops = ops;
	err = rvt_init(rdev);
	if (err)
		goto err1;
	for (i = 1; i <= rdev->num_ports; ++i) {
		err = rvt_set_mtu(rdev, mtu, i);
		if (err)
			goto err1;
	}

	dev = &rdev->ib_dev;
	strlcpy(dev->name, "rvt%d", IB_DEVICE_NAME_MAX);
	strlcpy(dev->node_desc, "rvt", sizeof(dev->node_desc));

	dev->owner = THIS_MODULE;
	dev->node_type = RDMA_NODE_IB_CA;
	dev->phys_port_cnt = rdev->num_ports;
	dev->num_comp_vectors = RVT_NUM_COMP_VECTORS;
	dev->dma_device = rdev->ifc_ops->dma_device(rdev);
	dev->local_dma_lkey = 0;
	dev->node_guid = rdev->ifc_ops->node_guid(rdev);
	dev->dma_ops = &rvt_dma_mapping_ops;

	dev->uverbs_abi_ver = RVT_UVERBS_ABI_VERSION;
	dev->uverbs_cmd_mask = BIT_ULL(IB_USER_VERBS_CMD_GET_CONTEXT)
	    | BIT_ULL(IB_USER_VERBS_CMD_CREATE_COMP_CHANNEL)
	    | BIT_ULL(IB_USER_VERBS_CMD_QUERY_DEVICE)
	    | BIT_ULL(IB_USER_VERBS_CMD_QUERY_PORT)
	    | BIT_ULL(IB_USER_VERBS_CMD_ALLOC_PD)
	    | BIT_ULL(IB_USER_VERBS_CMD_DEALLOC_PD)
	    | BIT_ULL(IB_USER_VERBS_CMD_CREATE_SRQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_MODIFY_SRQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_QUERY_SRQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_DESTROY_SRQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_POST_SRQ_RECV)
	    | BIT_ULL(IB_USER_VERBS_CMD_CREATE_QP)
	    | BIT_ULL(IB_USER_VERBS_CMD_MODIFY_QP)
	    | BIT_ULL(IB_USER_VERBS_CMD_QUERY_QP)
	    | BIT_ULL(IB_USER_VERBS_CMD_DESTROY_QP)
	    | BIT_ULL(IB_USER_VERBS_CMD_POST_SEND)
	    | BIT_ULL(IB_USER_VERBS_CMD_POST_RECV)
	    | BIT_ULL(IB_USER_VERBS_CMD_CREATE_CQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_RESIZE_CQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_DESTROY_CQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_PEEK_CQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ)
	    | BIT_ULL(IB_USER_VERBS_CMD_REG_MR)
	    | BIT_ULL(IB_USER_VERBS_CMD_DEREG_MR)
	    | BIT_ULL(IB_USER_VERBS_CMD_CREATE_AH)
	    | BIT_ULL(IB_USER_VERBS_CMD_MODIFY_AH)
	    | BIT_ULL(IB_USER_VERBS_CMD_QUERY_AH)
	    | BIT_ULL(IB_USER_VERBS_CMD_DESTROY_AH)
	    | BIT_ULL(IB_USER_VERBS_CMD_ATTACH_MCAST)
	    | BIT_ULL(IB_USER_VERBS_CMD_DETACH_MCAST)
	    ;

	dev->query_device = rvt_query_device;
	dev->modify_device = rvt_modify_device;
	dev->query_port = rvt_query_port;
	dev->modify_port = rvt_modify_port;
	dev->get_link_layer = rvt_get_link_layer;
	dev->query_gid = rvt_query_gid;
	dev->get_netdev = rvt_get_netdev;
	dev->add_gid = rvt_add_gid;
	dev->del_gid = rvt_del_gid;
	dev->query_pkey = rvt_query_pkey;
	dev->alloc_ucontext = rvt_alloc_ucontext;
	dev->dealloc_ucontext = rvt_dealloc_ucontext;
	dev->mmap = rvt_mmap;
	dev->get_port_immutable = rvt_port_immutable;
	dev->alloc_pd = rvt_alloc_pd;
	dev->dealloc_pd = rvt_dealloc_pd;
	dev->create_ah = rvt_create_ah;
	dev->modify_ah = rvt_modify_ah;
	dev->query_ah = rvt_query_ah;
	dev->destroy_ah = rvt_destroy_ah;
	dev->create_srq = rvt_create_srq;
	dev->modify_srq = rvt_modify_srq;
	dev->query_srq = rvt_query_srq;
	dev->destroy_srq = rvt_destroy_srq;
	dev->post_srq_recv = rvt_post_srq_recv;
	dev->create_qp = rvt_create_qp;
	dev->modify_qp = rvt_modify_qp;
	dev->query_qp = rvt_query_qp;
	dev->destroy_qp = rvt_destroy_qp;
	dev->post_send = rvt_post_send;
	dev->post_recv = rvt_post_recv;
	dev->create_cq = rvt_create_cq;
	dev->destroy_cq = rvt_destroy_cq;
	dev->resize_cq = rvt_resize_cq;
	dev->poll_cq = rvt_poll_cq;
	dev->peek_cq = rvt_peek_cq;
	dev->req_notify_cq = rvt_req_notify_cq;
	dev->get_dma_mr = rvt_get_dma_mr;
	dev->reg_user_mr = rvt_reg_user_mr;
	dev->dereg_mr = rvt_dereg_mr;
	dev->alloc_mr = rvt_alloc_mr;
	dev->alloc_mw = rvt_alloc_mw;
	dev->dealloc_mw = rvt_dealloc_mw;
	dev->alloc_fmr = rvt_alloc_fmr;
	dev->map_phys_fmr = rvt_map_phys_fmr;
	dev->unmap_fmr = rvt_unmap_fmr;
	dev->dealloc_fmr = rvt_dealloc_fmr;
	dev->attach_mcast = rvt_attach_mcast;
	dev->detach_mcast = rvt_detach_mcast;

	err = ib_register_device(dev, NULL);
	if (err) {
		pr_warn("rvt_register_device failed, err = %d\n", err);
		goto err1;
	}

	for (i = 0; i < ARRAY_SIZE(rvt_dev_attributes); ++i) {
		err = device_create_file(&dev->dev, rvt_dev_attributes[i]);
		if (err) {
			pr_warn("device_create_file failed, i = %d, err = %d\n",
				i, err);
			goto err2;
		}
	}

	return 0;

err2:
	ib_unregister_device(dev);
err1:
	rvt_dev_put(rdev);
	return err;
}
EXPORT_SYMBOL_GPL(rvt_register_device);

int rvt_unregister_device(struct rvt_dev *rdev)
{
	int i;
	struct ib_device *dev = &rdev->ib_dev;

	for (i = 0; i < ARRAY_SIZE(rvt_dev_attributes); ++i)
		device_remove_file(&dev->dev, rvt_dev_attributes[i]);

	ib_unregister_device(dev);

	rvt_dev_put(rdev);

	return 0;
}
EXPORT_SYMBOL_GPL(rvt_unregister_device);
