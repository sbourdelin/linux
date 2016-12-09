/*
 * Broadcom NetXtreme-E RoCE driver.
 *
 * Copyright (c) 2016, Broadcom. All rights reserved.  The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Description: Fast Path Operators (header)
 */

#ifndef __BNXT_QPLIB_FP_H__
#define __BNXT_QPLIB_FP_H__
#define BNXT_QPLIB_MAX_CQE_ENTRY_SIZE	sizeof(struct cq_base)

struct bnxt_qplib_cqe {
	u8				status;
	u8				type;
	u8				opcode;
	u32				length;
	u64				wr_id;
	u32				immdata_or_invrkey;
	u64				qp_handle;
	u64				mr_handle;
	u16				flags;
	u8				smac[6];
	u32				src_qp;
	u16				raweth_qp1_flags;
	u16				raweth_qp1_errors;
	u16				raweth_qp1_cfa_code;
	u32				raweth_qp1_flags2;
	u32				raweth_qp1_metadata;
	u8				raweth_qp1_payload_offset;
	u16				pkey_index;
};

#define BNXT_QPLIB_QUEUE_START_PERIOD		0x01
struct bnxt_qplib_cq {
	struct bnxt_qplib_dpi		*dpi;
	void __iomem			*dbr_base;
	u32				max_wqe;
	u32				id;
	u16				count;
	u16				period;
	struct bnxt_qplib_hwq		hwq;
	u32				cnq_hw_ring_id;
	bool				resize_in_progress;
	struct scatterlist		*sghead;
	u32				nmap;
	u64				cq_handle;

#define CQ_RESIZE_WAIT_TIME_MS		500
	unsigned long			flags;
#define CQ_FLAGS_RESIZE_IN_PROG		1
	wait_queue_head_t		waitq;
};

#define BNXT_QPLIB_MAX_NQE_ENTRY_SIZE	sizeof(struct nq_base)

#define NQE_CNT_PER_PG		(PAGE_SIZE / BNXT_QPLIB_MAX_NQE_ENTRY_SIZE)
#define NQE_MAX_IDX_PER_PG	(NQE_CNT_PER_PG - 1)
#define NQE_PG(x)		(((x) & ~NQE_MAX_IDX_PER_PG) / NQE_CNT_PER_PG)
#define NQE_IDX(x)		((x) & NQE_MAX_IDX_PER_PG)

#define NQE_CMP_VALID(hdr, raw_cons, cp_bit)			\
	(!!((hdr)->info63_v[0] & NQ_BASE_V) ==			\
	   !((raw_cons) & (cp_bit)))

#define BNXT_QPLIB_NQE_MAX_CNT		(128 * 1024)

#define NQ_CONS_PCI_BAR_REGION		2
#define NQ_DB_KEY_CP			(0x2 << CMPL_DOORBELL_KEY_SFT)
#define NQ_DB_IDX_VALID			CMPL_DOORBELL_IDX_VALID
#define NQ_DB_IRQ_DIS			CMPL_DOORBELL_MASK
#define NQ_DB_CP_FLAGS_REARM		(NQ_DB_KEY_CP |		\
					 NQ_DB_IDX_VALID)
#define NQ_DB_CP_FLAGS			(NQ_DB_KEY_CP    |	\
					 NQ_DB_IDX_VALID |	\
					 NQ_DB_IRQ_DIS)
#define NQ_DB_REARM(db, raw_cons, cp_bit)			\
	writel(NQ_DB_CP_FLAGS_REARM | ((raw_cons) & ((cp_bit) - 1)), db)
#define NQ_DB(db, raw_cons, cp_bit)				\
	writel(NQ_DB_CP_FLAGS | ((raw_cons) & ((cp_bit) - 1)), db)

struct bnxt_qplib_nq {
	struct pci_dev			*pdev;

	int				vector;
	int				budget;
	bool				requested;
	struct tasklet_struct		worker;
	struct bnxt_qplib_hwq		hwq;

	u16				bar_reg;
	u16				bar_reg_off;
	u16				ring_id;
	void __iomem			*bar_reg_iomem;

	int				(*cqn_handler)
						(struct bnxt_qplib_nq *nq,
						 void *cq);
	int				(*srqn_handler)
						(struct bnxt_qplib_nq *nq,
						 void *srq,
						 u8 event);
};

void bnxt_qplib_disable_nq(struct bnxt_qplib_nq *nq);
int bnxt_qplib_enable_nq(struct pci_dev *pdev, struct bnxt_qplib_nq *nq,
			 int msix_vector, int bar_reg_offset,
			 int (*cqn_handler)(struct bnxt_qplib_nq *nq,
					    void *cq),
			 int (*srqn_handler)(struct bnxt_qplib_nq *nq,
					     void *srq,
					     u8 event));
int bnxt_qplib_create_cq(struct bnxt_qplib_res *res, struct bnxt_qplib_cq *cq);
int bnxt_qplib_destroy_cq(struct bnxt_qplib_res *res, struct bnxt_qplib_cq *cq);

void bnxt_qplib_req_notify_cq(struct bnxt_qplib_cq *cq, u32 arm_type);
void bnxt_qplib_free_nq(struct bnxt_qplib_nq *nq);
int bnxt_qplib_alloc_nq(struct pci_dev *pdev, struct bnxt_qplib_nq *nq);
#endif /* __BNXT_QPLIB_FP_H__ */
