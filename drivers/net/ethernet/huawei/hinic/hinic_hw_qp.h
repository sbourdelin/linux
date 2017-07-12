/*
 * Huawei HiNIC PCI Express Linux driver
 * Copyright(c) 2017 Huawei Technologies Co., Ltd
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#ifndef HINIC_HW_QP_H
#define HINIC_HW_QP_H

#include <linux/types.h>
#include <linux/sizes.h>
#include <linux/pci.h>

#include "hinic_common.h"
#include "hinic_hw_if.h"
#include "hinic_hw_wq.h"
#include "hinic_hw_qp_ctxt.h"

#define HINIC_RQ_CQE_STATUS_RXDONE_SHIFT	31

#define HINIC_RQ_CQE_STATUS_RXDONE_MASK		0x1

#define HINIC_RQ_CQE_STATUS_GET(val, member)	\
		(((val) >> HINIC_RQ_CQE_STATUS_##member##_SHIFT) & \
		 HINIC_RQ_CQE_STATUS_##member##_MASK)

#define HINIC_RQ_CQE_STATUS_CLEAR(val, member)	\
		((val) & (~(HINIC_RQ_CQE_STATUS_##member##_MASK << \
		 HINIC_RQ_CQE_STATUS_##member##_SHIFT)))

#define HINIC_RQ_CQE_SGE_LEN_SHIFT		16

#define HINIC_RQ_CQE_SGE_LEN_MASK		0xFFFF

#define HINIC_RQ_CQE_SGE_GET(val, member)	\
		(((val) >> HINIC_RQ_CQE_SGE_##member##_SHIFT) & \
		 HINIC_RQ_CQE_SGE_##member##_MASK)

#define	HINIC_RQ_CTRL_BUFDESC_SECT_LEN_SHIFT	0
#define	HINIC_RQ_CTRL_COMPLETE_FORMAT_SHIFT	15
#define HINIC_RQ_CTRL_COMPLETE_LEN_SHIFT	27
#define HINIC_RQ_CTRL_LEN_SHIFT			29

#define	HINIC_RQ_CTRL_BUFDESC_SECT_LEN_MASK	0xFF
#define	HINIC_RQ_CTRL_COMPLETE_FORMAT_MASK	0x1
#define HINIC_RQ_CTRL_COMPLETE_LEN_MASK		0x3
#define HINIC_RQ_CTRL_LEN_MASK			0x3

#define HINIC_RQ_CTRL_SET(val, member)		\
		(((u32)(val) & HINIC_RQ_CTRL_##member##_MASK) << \
		 HINIC_RQ_CTRL_##member##_SHIFT)

#define HINIC_SQ_WQEBB_SIZE			64
#define HINIC_RQ_WQEBB_SIZE			32

#define HINIC_SQ_PAGE_SIZE			SZ_4K
#define HINIC_RQ_PAGE_SIZE			SZ_4K

#define HINIC_SQ_DEPTH				SZ_4K
#define HINIC_RQ_DEPTH				SZ_4K

#define HINIC_SQ_WQE_MAX_SIZE			320
#define HINIC_RQ_WQE_SIZE			32

#define HINIC_RX_BUF_SZ				2048

struct hinic_rq_cqe {
	u32	status;
	u32	len;

	u32	rsvd2;
	u32	rsvd3;
	u32	rsvd4;
	u32	rsvd5;
	u32	rsvd6;
	u32	rsvd7;
};

struct hinic_rq_ctrl {
	u32	ctrl_info;
};

struct hinic_rq_cqe_sect {
	struct hinic_sge	sge;
	u32			rsvd;
};

struct hinic_rq_bufdesc {
	u32	hi_addr;
	u32	lo_addr;
};

struct hinic_rq_wqe {
	struct hinic_rq_ctrl		ctrl;
	u32				rsvd;
	struct hinic_rq_cqe_sect	cqe_sect;
	struct hinic_rq_bufdesc		buf_desc;
};

struct hinic_sq {
	struct hinic_hwif	*hwif;

	struct hinic_wq		*wq;

	u32			irq;
	u16			msix_entry;

	void			*hw_ci_addr;
	dma_addr_t		hw_ci_dma_addr;

	void __iomem		*db_base;

	void			**priv;
};

struct hinic_rq {
	struct hinic_hwif	*hwif;

	struct hinic_wq		*wq;

	u32			irq;
	u16			msix_entry;

	size_t			buf_sz;

	void			**priv;

	struct hinic_rq_cqe	**cqe;
	dma_addr_t		*cqe_dma;

	u16			*pi_virt_addr;
	dma_addr_t		pi_dma_addr;
};

struct hinic_qp {
	struct hinic_sq		sq;
	struct hinic_rq		rq;

	u16	q_id;
};

void hinic_qp_prepare_header(struct hinic_qp_ctxt_header *qp_ctxt_hdr,
			     enum hinic_qp_ctxt_type ctxt_type,
			     int num_queues, int max_queues);

void hinic_sq_prepare_ctxt(struct hinic_sq *sq, u16 global_qid,
			   struct hinic_sq_ctxt *sq_ctxt);

void hinic_rq_prepare_ctxt(struct hinic_rq *rq, u16 global_qid,
			   struct hinic_rq_ctxt *rq_ctxt);

int hinic_init_sq(struct hinic_sq *sq, struct hinic_hwif *hwif,
		  struct hinic_wq *wq, struct msix_entry *entry, void *ci_addr,
		  dma_addr_t ci_dma_addr, void __iomem *db_base);

void hinic_clean_sq(struct hinic_sq *sq);

int hinic_init_rq(struct hinic_rq *rq, struct hinic_hwif *hwif,
		  struct hinic_wq *wq, struct msix_entry *entry);

void hinic_clean_rq(struct hinic_rq *rq);

int hinic_get_rq_free_wqebbs(struct hinic_rq *rq);

void *hinic_rq_get_wqe(struct hinic_rq *rq, unsigned int wqe_size,
		       u16 *prod_idx);

void hinic_rq_write_wqe(struct hinic_rq *rq, u16 prod_idx, void *wqe,
			void *priv);

void *hinic_rq_read_wqe(struct hinic_rq *rq, unsigned int wqe_size, void **priv,
			u16 *cons_idx);

void *hinic_rq_read_next_wqe(struct hinic_rq *rq, unsigned int wqe_size,
			     void **priv, u16 *cons_idx);

void hinic_rq_put_wqe(struct hinic_rq *rq, u16 cons_idx,
		      unsigned int wqe_size);

void hinic_rq_get_sge(struct hinic_rq *rq, void *wqe, u16 cons_idx,
		      struct hinic_sge *sge);

void hinic_rq_prepare_wqe(struct hinic_rq *rq, u16 prod_idx, void *wqe,
			  struct hinic_sge *sge);

void hinic_rq_update(struct hinic_rq *rq, u16 prod_idx);

#endif
