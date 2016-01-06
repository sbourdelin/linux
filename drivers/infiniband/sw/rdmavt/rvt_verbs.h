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

#ifndef RVT_VERBS_H
#define RVT_VERBS_H

#include <linux/interrupt.h>
#include <rdma/ib_rvt.h>
#include "rvt_pool.h"
#include "rvt_task.h"
#include "rvt_param.h"

#define RVT_UVERBS_ABI_VERSION		(1)
static inline int pkey_match(u16 key1, u16 key2)
{
	return (((key1 & 0x7fff) != 0) &&
		((key1 & 0x7fff) == (key2 & 0x7fff)) &&
		((key1 & 0x8000) || (key2 & 0x8000))) ? 1 : 0;
}

static inline int addr_same(struct rvt_dev *rdev, struct rvt_av *av)
{
	int port_num = 1;

	return rdev->port[port_num - 1].port_guid
			== av->grh.dgid.global.interface_id;
}

/* Return >0 if psn_a > psn_b
 *	   0 if psn_a == psn_b
 *	  <0 if psn_a < psn_b
 */
static inline int psn_compare(u32 psn_a, u32 psn_b)
{
	s32 diff;

	diff = (psn_a - psn_b) << 8;
	return diff;
}

struct rvt_ucontext {
	struct rvt_pool_entry	pelem;
	struct ib_ucontext	ibuc;
};

struct rvt_pd {
	struct rvt_pool_entry	pelem;
	struct ib_pd		ibpd;
};

struct rvt_ah {
	struct rvt_pool_entry	pelem;
	struct ib_ah		ibah;
	struct rvt_pd		*pd;
	struct rvt_av		av;
};

struct rvt_cqe {
	union {
		struct ib_wc		ibwc;
		struct ib_uverbs_wc	uibwc;
	};
};

struct rvt_cq {
	struct rvt_pool_entry	pelem;
	struct ib_cq		ibcq;
	struct rvt_queue	*queue;
	spinlock_t		cq_lock;
	u8			notify;
	int			is_user;
	struct tasklet_struct	comp_task;
};

enum wqe_state {
	wqe_state_posted,
	wqe_state_processing,
	wqe_state_pending,
	wqe_state_done,
	wqe_state_error,
};

struct rvt_sq {
	int			max_wr;
	int			max_sge;
	int			max_inline;
	spinlock_t		sq_lock;
	struct rvt_queue	*queue;
};

struct rvt_rq {
	int			max_wr;
	int			max_sge;
	spinlock_t		producer_lock;
	spinlock_t		consumer_lock;
	struct rvt_queue	*queue;
};

struct rvt_srq {
	struct rvt_pool_entry	pelem;
	struct ib_srq		ibsrq;
	struct rvt_pd		*pd;
	struct rvt_cq		*cq;
	struct rvt_rq		rq;
	u32			srq_num;

	void			(*event_handler)(
					struct ib_event *, void *);
	void			*context;

	int			limit;
	int			error;
};

enum rvt_qp_state {
	QP_STATE_RESET,
	QP_STATE_INIT,
	QP_STATE_READY,
	QP_STATE_DRAIN,		/* req only */
	QP_STATE_DRAINED,	/* req only */
	QP_STATE_ERROR
};

extern char *rvt_qp_state_name[];

struct rvt_req_info {
	enum rvt_qp_state	state;
	int			wqe_index;
	u32			psn;
	int			opcode;
	atomic_t		rd_atomic;
	int			wait_fence;
	int			need_rd_atomic;
	int			wait_psn;
	int			need_retry;
	int			noack_pkts;
	struct rvt_task		task;
};

struct rvt_comp_info {
	u32			psn;
	int			opcode;
	int			timeout;
	int			timeout_retry;
	u32			retry_cnt;
	u32			rnr_retry;
	struct rvt_task		task;
};

enum rdatm_res_state {
	rdatm_res_state_next,
	rdatm_res_state_new,
	rdatm_res_state_replay,
};

struct resp_res {
	int			type;
	u32			first_psn;
	u32			last_psn;
	u32			cur_psn;
	enum rdatm_res_state	state;

	union {
		struct {
			struct sk_buff	*skb;
		} atomic;
		struct {
			struct rvt_mem	*mr;
			u64		va_org;
			u32		rkey;
			u32		length;
			u64		va;
			u32		resid;
		} read;
	};
};

struct rvt_resp_info {
	enum rvt_qp_state	state;
	u32			msn;
	u32			psn;
	int			opcode;
	int			drop_msg;
	int			goto_error;
	int			sent_psn_nak;
	enum ib_wc_status	status;
	u8			aeth_syndrome;

	/* Receive only */
	struct rvt_recv_wqe	*wqe;

	/* RDMA read / atomic only */
	u64			va;
	struct rvt_mem		*mr;
	u32			resid;
	u32			rkey;
	u64			atomic_orig;

	/* SRQ only */
	struct {
		struct rvt_recv_wqe	wqe;
		struct ib_sge		sge[RVT_MAX_SGE];
	} srq_wqe;

	/* Responder resources. It's a circular list where the oldest
	 * resource is dropped first.
	 */
	struct resp_res		*resources;
	unsigned int		res_head;
	unsigned int		res_tail;
	struct resp_res		*res;
	struct rvt_task		task;
};

struct rvt_qp {
	struct rvt_pool_entry	pelem;
	struct ib_qp		ibqp;
	struct ib_qp_attr	attr;
	unsigned int		valid;
	unsigned int		mtu;
	int			is_user;

	struct rvt_pd		*pd;
	struct rvt_srq		*srq;
	struct rvt_cq		*scq;
	struct rvt_cq		*rcq;

	enum ib_sig_type	sq_sig_type;

	struct rvt_sq		sq;
	struct rvt_rq		rq;

	void			*flow;

	struct rvt_av		pri_av;
	struct rvt_av		alt_av;

	/* list of mcast groups qp has joined (for cleanup) */
	struct list_head	grp_list;
	spinlock_t		grp_lock;

	struct sk_buff_head	req_pkts;
	struct sk_buff_head	resp_pkts;
	struct sk_buff_head	send_pkts;

	struct rvt_req_info	req;
	struct rvt_comp_info	comp;
	struct rvt_resp_info	resp;

	atomic_t		ssn;
	atomic_t		skb_out;
	int			need_req_skb;

	/* Timer for retranmitting packet when ACKs have been lost. RC
	 * only. The requester sets it when it is not already
	 * started. The responder resets it whenever an ack is
	 * received.
	 */
	struct timer_list retrans_timer;
	u64 qp_timeout_jiffies;

	/* Timer for handling RNR NAKS. */
	struct timer_list rnr_nak_timer;

	spinlock_t		state_lock;
};

enum rvt_mem_state {
	RVT_MEM_STATE_ZOMBIE,
	RVT_MEM_STATE_INVALID,
	RVT_MEM_STATE_FREE,
	RVT_MEM_STATE_VALID,
};

enum rvt_mem_type {
	RVT_MEM_TYPE_NONE,
	RVT_MEM_TYPE_DMA,
	RVT_MEM_TYPE_MR,
	RVT_MEM_TYPE_FMR,
	RVT_MEM_TYPE_MW,
};

#define RVT_BUF_PER_MAP		(PAGE_SIZE / sizeof(struct rvt_phys_buf))

struct rvt_phys_buf {
	u64	addr;
	u64	size;
};

struct rvt_map {
	struct rvt_phys_buf	buf[RVT_BUF_PER_MAP];
};

struct rvt_mem {
	struct rvt_pool_entry	pelem;
	union {
		struct ib_mr		ibmr;
		struct ib_fmr		ibfmr;
		struct ib_mw		ibmw;
	};

	struct rvt_pd		*pd;
	struct ib_umem		*umem;

	u32			lkey;
	u32			rkey;

	enum rvt_mem_state	state;
	enum rvt_mem_type	type;
	u64			va;
	u64			iova;
	size_t			length;
	u32			offset;
	int			access;

	int			page_shift;
	int			page_mask;
	int			map_shift;
	int			map_mask;

	u32			num_buf;

	u32			max_buf;
	u32			num_map;

	struct rvt_map		**map;
};

struct rvt_mc_grp {
	struct rvt_pool_entry	pelem;
	spinlock_t		mcg_lock;
	struct rvt_dev		*rvt;
	struct list_head	qp_list;
	union ib_gid		mgid;
	int			num_qp;
	u32			qkey;
	u16			pkey;
};

struct rvt_mc_elem {
	struct rvt_pool_entry	pelem;
	struct list_head	qp_list;
	struct list_head	grp_list;
	struct rvt_qp		*qp;
	struct rvt_mc_grp	*grp;
};

int rvt_prepare(struct rvt_dev *rvt, struct rvt_pkt_info *pkt,
		   struct sk_buff *skb, u32 *crc);

static inline struct rvt_dev *to_rdev(struct ib_device *dev)
{
	return dev ? container_of(dev, struct rvt_dev, ib_dev) : NULL;
}

static inline struct rvt_ucontext *to_ruc(struct ib_ucontext *uc)
{
	return uc ? container_of(uc, struct rvt_ucontext, ibuc) : NULL;
}

static inline struct rvt_pd *to_rpd(struct ib_pd *pd)
{
	return pd ? container_of(pd, struct rvt_pd, ibpd) : NULL;
}

static inline struct rvt_ah *to_rah(struct ib_ah *ah)
{
	return ah ? container_of(ah, struct rvt_ah, ibah) : NULL;
}

static inline struct rvt_srq *to_rsrq(struct ib_srq *srq)
{
	return srq ? container_of(srq, struct rvt_srq, ibsrq) : NULL;
}

static inline struct rvt_qp *to_rqp(struct ib_qp *qp)
{
	return qp ? container_of(qp, struct rvt_qp, ibqp) : NULL;
}

static inline struct rvt_cq *to_rcq(struct ib_cq *cq)
{
	return cq ? container_of(cq, struct rvt_cq, ibcq) : NULL;
}

static inline struct rvt_mem *to_rmr(struct ib_mr *mr)
{
	return mr ? container_of(mr, struct rvt_mem, ibmr) : NULL;
}

static inline struct rvt_mem *to_rfmr(struct ib_fmr *fmr)
{
	return fmr ? container_of(fmr, struct rvt_mem, ibfmr) : NULL;
}

static inline struct rvt_mem *to_rmw(struct ib_mw *mw)
{
	return mw ? container_of(mw, struct rvt_mem, ibmw) : NULL;
}


void rvt_mc_cleanup(void *arg);

#endif /* RVT_VERBS_H */
