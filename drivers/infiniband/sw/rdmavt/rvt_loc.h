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

#ifndef RVT_LOC_H
#define RVT_LOC_H
#include <rdma/ib_verbs.h>
#include <rdma/ib_rvt.h>
#include <uapi/rdma/ib_user_rvt.h>


#include "rvt_task.h"
#include "rvt_verbs.h"
#include "rvt_param.h"
#include "rvt_hdr.h"
#include "rvt_opcode.h"


/* rvt_av.c */
int rvt_av_chk_attr(struct rvt_dev *rvt, struct ib_ah_attr *attr);

int rvt_av_from_attr(struct rvt_dev *rvt, u8 port_num,
		     struct rvt_av *av, struct ib_ah_attr *attr);

int rvt_av_to_attr(struct rvt_dev *rvt, struct rvt_av *av,
		   struct ib_ah_attr *attr);

int rvt_av_fill_ip_info(struct rvt_dev *rvt,
			struct rvt_av *av,
			struct ib_ah_attr *attr,
			struct ib_gid_attr *sgid_attr,
			union ib_gid *sgid);

/* rvt_cq.c */
int rvt_cq_chk_attr(struct rvt_dev *rvt, struct rvt_cq *cq,
		    int cqe, int comp_vector, struct ib_udata *udata);

int rvt_cq_from_init(struct rvt_dev *rvt, struct rvt_cq *cq, int cqe,
		     int comp_vector, struct ib_ucontext *context,
		     struct ib_udata *udata);

int rvt_cq_resize_queue(struct rvt_cq *cq, int new_cqe, struct ib_udata *udata);

int rvt_cq_post(struct rvt_cq *cq, struct rvt_cqe *cqe, int solicited);

void rvt_cq_cleanup(void *arg);

/* rvt_mcast.c */
int rvt_mcast_get_grp(struct rvt_dev *rvt, union ib_gid *mgid,
		      struct rvt_mc_grp **grp_p);

int rvt_mcast_add_grp_elem(struct rvt_dev *rvt, struct rvt_qp *qp,
			   struct rvt_mc_grp *grp);

int rvt_mcast_drop_grp_elem(struct rvt_dev *rvt, struct rvt_qp *qp,
			    union ib_gid *mgid);

void rvt_drop_all_mcast_groups(struct rvt_qp *qp);

void rvt_mc_cleanup(void *arg);

/* rvt_mmap.c */
struct rvt_mmap_info {
	struct list_head	pending_mmaps;
	struct ib_ucontext	*context;
	struct kref		ref;
	void			*obj;

	struct mminfo info;
};

void rvt_mmap_release(struct kref *ref);

struct rvt_mmap_info *rvt_create_mmap_info(struct rvt_dev *dev,
					   u32 size,
					   struct ib_ucontext *context,
					   void *obj);

int rvt_mmap(struct ib_ucontext *context, struct vm_area_struct *vma);

/* rvt_mr.c */
enum copy_direction {
	to_mem_obj,
	from_mem_obj,
};

int rvt_mem_init_dma(struct rvt_dev *rvt, struct rvt_pd *pd,
		     int access, struct rvt_mem *mem);

int rvt_mem_init_phys(struct rvt_dev *rvt, struct rvt_pd *pd,
		      int access, u64 iova, struct rvt_phys_buf *buf,
		      int num_buf, struct rvt_mem *mem);

int rvt_mem_init_user(struct rvt_dev *rvt, struct rvt_pd *pd, u64 start,
		      u64 length, u64 iova, int access, struct ib_udata *udata,
		      struct rvt_mem *mr);

int rvt_mem_init_fast(struct rvt_dev *rvt, struct rvt_pd *pd,
		      int max_pages, struct rvt_mem *mem);

int rvt_mem_init_mw(struct rvt_dev *rvt, struct rvt_pd *pd,
		    struct rvt_mem *mw);

int rvt_mem_init_fmr(struct rvt_dev *rvt, struct rvt_pd *pd, int access,
		     struct ib_fmr_attr *attr, struct rvt_mem *fmr);

int rvt_mem_copy(struct rvt_mem *mem, u64 iova, void *addr,
		 int length, enum copy_direction dir, u32 *crcp);

int copy_data(struct rvt_dev *rvt, struct rvt_pd *pd, int access,
	      struct rvt_dma_info *dma, void *addr, int length,
	      enum copy_direction dir, u32 *crcp);

void *iova_to_vaddr(struct rvt_mem *mem, u64 iova, int length);

enum lookup_type {
	lookup_local,
	lookup_remote,
};

struct rvt_mem *lookup_mem(struct rvt_pd *pd, int access, u32 key,
			   enum lookup_type type);

int mem_check_range(struct rvt_mem *mem, u64 iova, size_t length);

int rvt_mem_map_pages(struct rvt_dev *rvt, struct rvt_mem *mem,
		      u64 *page, int num_pages, u64 iova);

void rvt_mem_cleanup(void *arg);

int advance_dma_data(struct rvt_dma_info *dma, unsigned int length);

/* rvt_qp.c */
int rvt_qp_chk_init(struct rvt_dev *rvt, struct ib_qp_init_attr *init);

int rvt_qp_from_init(struct rvt_dev *rvt, struct rvt_qp *qp, struct rvt_pd *pd,
		     struct ib_qp_init_attr *init, struct ib_udata *udata,
		     struct ib_pd *ibpd);

int rvt_qp_to_init(struct rvt_qp *qp, struct ib_qp_init_attr *init);

int rvt_qp_chk_attr(struct rvt_dev *rvt, struct rvt_qp *qp,
		    struct ib_qp_attr *attr, int mask);

int rvt_qp_from_attr(struct rvt_qp *qp, struct ib_qp_attr *attr,
		     int mask, struct ib_udata *udata);

int rvt_qp_to_attr(struct rvt_qp *qp, struct ib_qp_attr *attr, int mask);

void rvt_qp_error(struct rvt_qp *qp);

void rvt_qp_destroy(struct rvt_qp *qp);

void rvt_qp_cleanup(void *arg);

static inline int qp_num(struct rvt_qp *qp)
{
	return qp->ibqp.qp_num;
}

static inline enum ib_qp_type qp_type(struct rvt_qp *qp)
{
	return qp->ibqp.qp_type;
}

static struct rvt_av *get_av(struct rvt_pkt_info *pkt)
{
	if (qp_type(pkt->qp) == IB_QPT_RC || qp_type(pkt->qp) == IB_QPT_UC)
		return &pkt->qp->pri_av;

	return &pkt->wqe->av;
}

static inline enum ib_qp_state qp_state(struct rvt_qp *qp)
{
	return qp->attr.qp_state;
}

static inline int qp_mtu(struct rvt_qp *qp)
{
	if (qp->ibqp.qp_type == IB_QPT_RC || qp->ibqp.qp_type == IB_QPT_UC)
		return qp->attr.path_mtu;
	else
		return RVT_PORT_MAX_MTU;
}

static inline int rcv_wqe_size(int max_sge)
{
	return sizeof(struct rvt_recv_wqe) +
		max_sge * sizeof(struct ib_sge);
}

void free_rd_atomic_resource(struct rvt_qp *qp, struct resp_res *res);

static inline void rvt_advance_resp_resource(struct rvt_qp *qp)
{
	qp->resp.res_head++;
	if (unlikely(qp->resp.res_head == qp->attr.max_rd_atomic))
		qp->resp.res_head = 0;
}

void retransmit_timer(unsigned long data);
void rnr_nak_timer(unsigned long data);

void dump_qp(struct rvt_qp *qp);

/* rvt_srq.c */
#define IB_SRQ_INIT_MASK (~IB_SRQ_LIMIT)

int rvt_srq_chk_attr(struct rvt_dev *rvt, struct rvt_srq *srq,
		     struct ib_srq_attr *attr, enum ib_srq_attr_mask mask);

int rvt_srq_from_init(struct rvt_dev *rvt, struct rvt_srq *srq,
		      struct ib_srq_init_attr *init,
		      struct ib_ucontext *context, struct ib_udata *udata);

int rvt_srq_from_attr(struct rvt_dev *rvt, struct rvt_srq *srq,
		      struct ib_srq_attr *attr, enum ib_srq_attr_mask mask,
		      struct ib_udata *udata);

extern struct ib_dma_mapping_ops rvt_dma_mapping_ops;

void rvt_release(struct kref *kref);

int rvt_completer(void *arg);
int rvt_requester(void *arg);
int rvt_responder(void *arg);

u32 rvt_icrc_hdr(struct rvt_pkt_info *pkt, struct sk_buff *skb);

void rvt_resp_queue_pkt(struct rvt_dev *rvt,
			struct rvt_qp *qp, struct sk_buff *skb);

void rvt_comp_queue_pkt(struct rvt_dev *rvt,
			struct rvt_qp *qp, struct sk_buff *skb);

static inline unsigned wr_opcode_mask(int opcode, struct rvt_qp *qp)
{
	return rvt_wr_opcode_info[opcode].mask[qp->ibqp.qp_type];
}

static inline int rvt_xmit_packet(struct rvt_dev *rvt, struct rvt_qp *qp,
				  struct rvt_pkt_info *pkt, struct sk_buff *skb)
{
	int err;
	int is_request = pkt->mask & RVT_REQ_MASK;

	if ((is_request && (qp->req.state != QP_STATE_READY)) ||
	    (!is_request && (qp->resp.state != QP_STATE_READY))) {
		pr_info("Packet dropped. QP is not in ready state\n");
		goto drop;
	}

	if (pkt->mask & RVT_LOOPBACK_MASK)
		err = rvt->ifc_ops->loopback(skb);
	else
		err = rvt->ifc_ops->send(rvt, get_av(pkt), skb, qp->flow);

	if (err) {
		rvt->xmit_errors++;
		return err;
	}

	atomic_inc(&qp->skb_out);

	if ((qp_type(qp) != IB_QPT_RC) &&
	    (pkt->mask & RVT_END_MASK)) {
		pkt->wqe->state = wqe_state_done;
		rvt_run_task(&qp->comp.task, 1);
	}

	goto done;

drop:
	kfree_skb(skb);
	err = 0;
done:
	return err;
}

#endif /* RVT_LOC_H */
