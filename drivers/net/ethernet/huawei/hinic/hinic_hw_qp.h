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

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/sizes.h>
#include <linux/pci.h>

#include "hinic_common.h"
#include "hinic_hw_if.h"
#include "hinic_hw_wq.h"
#include "hinic_hw_qp_ctxt.h"

#define	HINIC_SQ_CTRL_BUFDESC_SECT_LEN_SHIFT	0
#define HINIC_SQ_CTRL_TASKSECT_LEN_SHIFT	16
#define	HINIC_SQ_CTRL_DATA_FORMAT_SHIFT		22
#define HINIC_SQ_CTRL_LEN_SHIFT			29

#define	HINIC_SQ_CTRL_BUFDESC_SECT_LEN_MASK	0xFF
#define HINIC_SQ_CTRL_TASKSECT_LEN_MASK		0x1F
#define	HINIC_SQ_CTRL_DATA_FORMAT_MASK		0x1
#define HINIC_SQ_CTRL_LEN_MASK			0x3

#define HINIC_SQ_CTRL_QUEUE_INFO_MSS_SHIFT	13

#define HINIC_SQ_CTRL_QUEUE_INFO_MSS_MASK	0x3FFF

#define HINIC_SQ_CTRL_SET(val, member)		\
		(((u32)(val) & HINIC_SQ_CTRL_##member##_MASK) \
		 << HINIC_SQ_CTRL_##member##_SHIFT)

#define HINIC_SQ_CTRL_GET(val, member)		\
		(((val) >> HINIC_SQ_CTRL_##member##_SHIFT) \
		 & HINIC_SQ_CTRL_##member##_MASK)

#define HINIC_SQ_TASK_INFO0_L2HDR_LEN_SHIFT	0
#define HINIC_SQ_TASK_INFO0_L4_OFFLOAD_SHIFT	8
#define HINIC_SQ_TASK_INFO0_INNER_L3TYPE_SHIFT	10
#define HINIC_SQ_TASK_INFO0_VLAN_OFFLOAD_SHIFT	12
#define HINIC_SQ_TASK_INFO0_PARSE_FLAG_SHIFT	13
/* 1 bit reserved */
#define HINIC_SQ_TASK_INFO0_TSO_FLAG_SHIFT	15
#define HINIC_SQ_TASK_INFO0_VLAN_TAG_SHIFT	16

#define HINIC_SQ_TASK_INFO0_L2HDR_LEN_MASK	0xFF
#define HINIC_SQ_TASK_INFO0_L4_OFFLOAD_MASK	0x3
#define HINIC_SQ_TASK_INFO0_INNER_L3TYPE_MASK	0x3
#define HINIC_SQ_TASK_INFO0_VLAN_OFFLOAD_MASK	0x1
#define HINIC_SQ_TASK_INFO0_PARSE_FLAG_MASK	0x1
/* 1 bit reserved */
#define HINIC_SQ_TASK_INFO0_TSO_FLAG_MASK	0x1
#define HINIC_SQ_TASK_INFO0_VLAN_TAG_MASK	0xFFFF

#define HINIC_SQ_TASK_INFO0_SET(val, member)	\
		(((u32)(val) & HINIC_SQ_TASK_INFO0_##member##_MASK) <<	\
		 HINIC_SQ_TASK_INFO0_##member##_SHIFT)

/* 8 bits reserved */
#define HINIC_SQ_TASK_INFO1_MEDIA_TYPE_SHIFT	8
#define HINIC_SQ_TASK_INFO1_INNER_L4_LEN_SHIFT	16
#define HINIC_SQ_TASK_INFO1_INNER_L3_LEN_SHIFT	24

/* 8 bits reserved */
#define HINIC_SQ_TASK_INFO1_MEDIA_TYPE_MASK	0xFF
#define HINIC_SQ_TASK_INFO1_INNER_L4_LEN_MASK	0xFF
#define HINIC_SQ_TASK_INFO1_INNER_L3_LEN_MASK	0xFF

#define HINIC_SQ_TASK_INFO1_SET(val, member)	\
		(((u32)(val) & HINIC_SQ_TASK_INFO1_##member##_MASK) <<	\
		 HINIC_SQ_TASK_INFO1_##member##_SHIFT)

#define HINIC_SQ_TASK_INFO2_TUNNEL_L4_LEN_SHIFT	0
#define HINIC_SQ_TASK_INFO2_OUTER_L3_LEN_SHIFT	12
#define HINIC_SQ_TASK_INFO2_TUNNEL_L4TYPE_SHIFT	19
/* 1 bit reserved */
#define HINIC_SQ_TASK_INFO2_OUTER_L3TYPE_SHIFT	22
/* 8 bits reserved */

#define HINIC_SQ_TASK_INFO2_TUNNEL_L4_LEN_MASK	0xFFF
#define HINIC_SQ_TASK_INFO2_OUTER_L3_LEN_MASK	0x7F
#define HINIC_SQ_TASK_INFO2_TUNNEL_L4TYPE_MASK	0x3
/* 1 bit reserved */
#define HINIC_SQ_TASK_INFO2_OUTER_L3TYPE_MASK	0x3
/* 8 bits reserved */

#define HINIC_SQ_TASK_INFO2_SET(val, member)	\
		(((u32)(val) & HINIC_SQ_TASK_INFO2_##member##_MASK) <<	\
		 HINIC_SQ_TASK_INFO2_##member##_SHIFT)

/* 31 bits reserved */
#define HINIC_SQ_TASK_INFO4_L2TYPE_SHIFT	31

/* 31 bits reserved */
#define HINIC_SQ_TASK_INFO4_L2TYPE_MASK		0x1

#define HINIC_SQ_TASK_INFO4_SET(val, member)	\
		(((u32)(val) & HINIC_SQ_TASK_INFO4_##member##_MASK) << \
		 HINIC_SQ_TASK_INFO4_##member##_SHIFT)

#define HINIC_SQ_DB_INFO_PI_HI_SHIFT		0
#define HINIC_SQ_DB_INFO_QID_SHIFT		8
#define HINIC_SQ_DB_INFO_PATH_SHIFT		23
#define HINIC_SQ_DB_INFO_COS_SHIFT		24
#define HINIC_SQ_DB_INFO_TYPE_SHIFT		27

#define HINIC_SQ_DB_INFO_PI_HI_MASK		0xFF
#define HINIC_SQ_DB_INFO_QID_MASK		0x3FF
#define HINIC_SQ_DB_INFO_PATH_MASK		0x1
#define HINIC_SQ_DB_INFO_COS_MASK		0x7
#define HINIC_SQ_DB_INFO_TYPE_MASK		0x1F

#define HINIC_SQ_DB_INFO_SET(val, member)	\
		(((u32)(val) & HINIC_SQ_DB_INFO_##member##_MASK) \
		 << HINIC_SQ_DB_INFO_##member##_SHIFT)

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

#define HINIC_MAX_SQ_BUFDESCS			17

#define HINIC_SQ_WQE_SIZE(nr_sges)		\
		(sizeof(struct hinic_sq_ctrl) + \
		 sizeof(struct hinic_sq_task) + \
		 (nr_sges) * sizeof(struct hinic_sq_bufdesc))

#define HINIC_MIN_TX_WQE_SIZE(wq)		\
		ALIGN(HINIC_SQ_WQE_SIZE(1), (wq)->wqebb_size)

#define HINIC_MIN_TX_NUM_WQEBBS(sq)		\
		(HINIC_MIN_TX_WQE_SIZE((sq)->wq) / (sq)->wq->wqebb_size)

enum hinic_l4offload_type {
	HINIC_L4_OFF_DISABLE   = 0,
	HINIC_TCP_OFFLOAD_ENABLE  = 1,
	HINIC_SCTP_OFFLOAD_ENABLE = 2,
	HINIC_UDP_OFFLOAD_ENABLE  = 3,
};

enum hinic_vlan_offload {
	HINIC_VLAN_OFF_DISABLE = 0,
	HINIC_VLAN_OFF_ENABLE  = 1,
};

enum hinic_pkt_parsed {
	HINIC_PKT_NOT_PARSED = 0,
	HINIC_PKT_PARSED     = 1,
};

enum hinic_outer_l3type {
	HINIC_OUTER_L3TYPE_UNKNOWN = 0,
	HINIC_OUTER_L3TYPE_IPV6 = 1,
	HINIC_OUTER_L3TYPE_IPV4_NO_CHKSUM = 2,
	HINIC_OUTER_L3TYPE_IPV4_CHKSUM = 3,
};

enum hinic_media_type {
	HINIC_MEDIA_UNKNOWN = 0,
};

enum hinic_l2type {
	HINIC_L2TYPE_ETH = 0,
};

enum hinc_tunnel_l4type {
	HINIC_TUNNEL_L4TYPE_UNKNOWN = 0,
};

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

struct hinic_sq_ctrl {
	u32	ctrl_info;
	u32	queue_info;
};

struct hinic_sq_task {
	u32	pkt_info0;
	u32	pkt_info1;
	u32	pkt_info2;
	u32	ufo_v6_identify;
	u32	pkt_info4;
	u32	zero_pad;
};

struct hinic_sq_bufdesc {
	struct hinic_sge sge;
	u32	rsvd;
};

struct hinic_sq_wqe {
	struct hinic_sq_ctrl		ctrl;
	struct hinic_sq_task		task;
	struct hinic_sq_bufdesc		buf_descs[HINIC_MAX_SQ_BUFDESCS];
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

int hinic_get_sq_free_wqebbs(struct hinic_sq *sq);

int hinic_get_rq_free_wqebbs(struct hinic_rq *rq);

void hinic_sq_prepare_wqe(struct hinic_sq *sq, u16 prod_idx, void *wqe,
			  struct hinic_sge *sges, int nr_sges);

void hinic_sq_write_db(struct hinic_sq *sq, u16 prod_idx, unsigned int cos);

void *hinic_sq_get_wqe(struct hinic_sq *sq, unsigned int wqe_size,
		       u16 *prod_idx);

void hinic_sq_write_wqe(struct hinic_sq *sq, u16 prod_idx, void *wqe,
			void *priv, unsigned int wqe_size);

void *hinic_sq_read_wqe(struct hinic_sq *sq, void **priv,
			unsigned int *wqe_size, u16 *cons_idx);

void hinic_sq_put_wqe(struct hinic_sq *sq, unsigned int wqe_size);

void hinic_sq_get_sges(void *wqe, struct hinic_sge *sges, int nr_sges);

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
