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

#ifndef HINIC_CMDQ_H
#define HINIC_CMDQ_H

#include <linux/types.h>
#include <linux/spinlock.h>
#include <linux/completion.h>
#include <linux/pci.h>

#include "hinic_common.h"
#include "hinic_hw_if.h"
#include "hinic_hw_wq.h"

#define HINIC_CMDQ_CTXT_CURR_WQE_PAGE_PFN_SHIFT		0
#define HINIC_CMDQ_CTXT_EQ_ID_SHIFT			56
#define HINIC_CMDQ_CTXT_CEQ_ARM_SHIFT			61
#define HINIC_CMDQ_CTXT_CEQ_EN_SHIFT			62
#define HINIC_CMDQ_CTXT_WRAPPED_SHIFT			63

#define HINIC_CMDQ_CTXT_CURR_WQE_PAGE_PFN_MASK		0xFFFFFFFFFFFFF
#define HINIC_CMDQ_CTXT_EQ_ID_MASK			0x1F
#define HINIC_CMDQ_CTXT_CEQ_ARM_MASK			0x1
#define HINIC_CMDQ_CTXT_CEQ_EN_MASK			0x1
#define HINIC_CMDQ_CTXT_WRAPPED_MASK			0x1

#define HINIC_CMDQ_CTXT_PAGE_INFO_SET(val, member)	\
			(((u64)(val) & HINIC_CMDQ_CTXT_##member##_MASK) \
			 << HINIC_CMDQ_CTXT_##member##_SHIFT)

#define HINIC_CMDQ_CTXT_PAGE_INFO_CLEAR(val, member)	\
			((val) & (~((u64)HINIC_CMDQ_CTXT_##member##_MASK \
			 << HINIC_CMDQ_CTXT_##member##_SHIFT)))

#define HINIC_CMDQ_CTXT_WQ_BLOCK_PFN_SHIFT		0
#define HINIC_CMDQ_CTXT_CI_SHIFT			52

#define HINIC_CMDQ_CTXT_WQ_BLOCK_PFN_MASK		0xFFFFFFFFFFFFF
#define HINIC_CMDQ_CTXT_CI_MASK				0xFFF

#define HINIC_CMDQ_CTXT_BLOCK_INFO_SET(val, member)	\
			(((u64)(val) & HINIC_CMDQ_CTXT_##member##_MASK) \
			 << HINIC_CMDQ_CTXT_##member##_SHIFT)

#define HINIC_CMDQ_CTXT_BLOCK_INFO_CLEAR(val, member)	\
			((val) & (~((u64)HINIC_CMDQ_CTXT_##member##_MASK \
			 << HINIC_CMDQ_CTXT_##member##_SHIFT)))

#define HINIC_CMDQ_CTRL_PI_SHIFT			0
#define HINIC_CMDQ_CTRL_CMD_SHIFT			16
#define HINIC_CMDQ_CTRL_MOD_SHIFT			24
#define HINIC_CMDQ_CTRL_ACK_TYPE_SHIFT			29
#define HINIC_CMDQ_CTRL_HW_BUSY_BIT_SHIFT		31

#define HINIC_CMDQ_CTRL_PI_MASK				0xFFFF
#define HINIC_CMDQ_CTRL_CMD_MASK			0xFF
#define HINIC_CMDQ_CTRL_MOD_MASK			0x1F
#define HINIC_CMDQ_CTRL_ACK_TYPE_MASK			0x3
#define HINIC_CMDQ_CTRL_HW_BUSY_BIT_MASK		0x1

#define HINIC_CMDQ_CTRL_SET(val, member)			\
			(((u32)(val) & HINIC_CMDQ_CTRL_##member##_MASK) \
			 << HINIC_CMDQ_CTRL_##member##_SHIFT)

#define HINIC_CMDQ_CTRL_GET(val, member)			\
			(((val) >> HINIC_CMDQ_CTRL_##member##_SHIFT) \
			 & HINIC_CMDQ_CTRL_##member##_MASK)

#define HINIC_CMDQ_WQE_HEADER_BUFDESC_LEN_SHIFT		0
#define HINIC_CMDQ_WQE_HEADER_COMPLETE_FMT_SHIFT	15
#define HINIC_CMDQ_WQE_HEADER_DATA_FMT_SHIFT		22
#define HINIC_CMDQ_WQE_HEADER_COMPLETE_REQ_SHIFT	23
#define HINIC_CMDQ_WQE_HEADER_COMPLETE_SECT_LEN_SHIFT	27
#define HINIC_CMDQ_WQE_HEADER_CTRL_LEN_SHIFT		29
#define HINIC_CMDQ_WQE_HEADER_TOGGLED_WRAPPED_SHIFT	31

#define HINIC_CMDQ_WQE_HEADER_BUFDESC_LEN_MASK		0xFF
#define HINIC_CMDQ_WQE_HEADER_COMPLETE_FMT_MASK		0x1
#define HINIC_CMDQ_WQE_HEADER_DATA_FMT_MASK		0x1
#define HINIC_CMDQ_WQE_HEADER_COMPLETE_REQ_MASK		0x1
#define HINIC_CMDQ_WQE_HEADER_COMPLETE_SECT_LEN_MASK	0x3
#define HINIC_CMDQ_WQE_HEADER_CTRL_LEN_MASK		0x3
#define HINIC_CMDQ_WQE_HEADER_TOGGLED_WRAPPED_MASK	0x1

#define HINIC_CMDQ_WQE_HEADER_SET(val, member)			\
			(((u32)(val) & HINIC_CMDQ_WQE_HEADER_##member##_MASK) \
			 << HINIC_CMDQ_WQE_HEADER_##member##_SHIFT)

#define HINIC_CMDQ_WQE_HEADER_GET(val, member)			\
			(((val) >> HINIC_CMDQ_WQE_HEADER_##member##_SHIFT) \
			 & HINIC_CMDQ_WQE_HEADER_##member##_MASK)

#define HINIC_SAVED_DATA_ARM_SHIFT			31

#define HINIC_SAVED_DATA_ARM_MASK			0x1

#define HINIC_SAVED_DATA_SET(val, member)		\
			(((u32)(val) & HINIC_SAVED_DATA_##member##_MASK) \
			 << HINIC_SAVED_DATA_##member##_SHIFT)

#define HINIC_SAVED_DATA_GET(val, member)		\
			(((val) >> HINIC_SAVED_DATA_##member##_SHIFT) \
			 & HINIC_SAVED_DATA_##member##_MASK)

#define HINIC_SAVED_DATA_CLEAR(val, member)		\
			((val) & (~(HINIC_SAVED_DATA_##member##_MASK \
			 << HINIC_SAVED_DATA_##member##_SHIFT)))

#define HINIC_CMDQ_DB_INFO_HI_PROD_IDX_SHIFT		0
#define HINIC_CMDQ_DB_INFO_PATH_SHIFT			23
#define HINIC_CMDQ_DB_INFO_CMDQ_TYPE_SHIFT		24
#define HINIC_CMDQ_DB_INFO_DB_TYPE_SHIFT		27

#define HINIC_CMDQ_DB_INFO_HI_PROD_IDX_MASK		0xFF
#define HINIC_CMDQ_DB_INFO_PATH_MASK			0x1
#define HINIC_CMDQ_DB_INFO_CMDQ_TYPE_MASK		0x7
#define HINIC_CMDQ_DB_INFO_DB_TYPE_MASK			0x1F

#define HINIC_CMDQ_DB_INFO_SET(val, member)		\
			(((u32)(val) & HINIC_CMDQ_DB_INFO_##member##_MASK) \
			 << HINIC_CMDQ_DB_INFO_##member##_SHIFT)

#define	HINIC_CMDQ_BUF_SIZE		2048

#define HINIC_CMDQ_BUF_HW_RSVD		8
#define HINIC_CMDQ_MAX_DATA_SIZE	(HINIC_CMDQ_BUF_SIZE - \
					 HINIC_CMDQ_BUF_HW_RSVD)

#define HINIC_SCMD_DATA_LEN		16

enum hinic_cmdq_type {
	HINIC_CMDQ_SYNC,

	HINIC_MAX_CMDQ_TYPES,
};

enum hinic_cmd_ack_type {
	HINIC_CMD_ACK_TYPE_CMDQ,
};

struct hinic_cmdq_buf {
	void		*buf;
	dma_addr_t	dma_addr;
	size_t		size;
};

struct hinic_cmdq_header {
	u32	header_info;
	u32	saved_data;
};

struct hinic_status {
	u32 status_info;
};

struct hinic_ctrl {
	u32 ctrl_info;
};

struct hinic_sge_resp {
	struct hinic_sge	sge;
	u32			rsvd;
};

struct hinic_cmdq_completion {
	/* HW Format */
	union {
		struct hinic_sge_resp	sge_resp;
		u64			direct_resp;
	};
};

struct hinic_scmd_bufdesc {
	u32	buf_len;
	u32	rsvd;
	u8	data[HINIC_SCMD_DATA_LEN];
};

struct hinic_lcmd_bufdesc {
	struct hinic_sge	sge;
	u32			rsvd1;
	u64			rsvd2;
	u64			rsvd3;
};

struct hinic_cmdq_wqe_scmd {
	struct hinic_cmdq_header	header;
	u64				rsvd;
	struct hinic_status		status;
	struct hinic_ctrl		ctrl;
	struct hinic_cmdq_completion	completion;
	struct hinic_scmd_bufdesc	buf_desc;
};

struct hinic_cmdq_wqe_lcmd {
	struct hinic_cmdq_header	header;
	struct hinic_status		status;
	struct hinic_ctrl		ctrl;
	struct hinic_cmdq_completion	completion;
	struct hinic_lcmd_bufdesc	buf_desc;
};

struct hinic_cmdq_direct_wqe {
	struct hinic_cmdq_wqe_scmd	wqe_scmd;
};

struct hinic_cmdq_wqe {
	/* HW Format */
	union {
		struct hinic_cmdq_direct_wqe	direct_wqe;
		struct hinic_cmdq_wqe_lcmd	wqe_lcmd;
	};
};

struct hinic_cmdq_ctxt_info {
	u64	curr_wqe_page_pfn;
	u64	wq_block_pfn;
};

struct hinic_cmdq_ctxt {
	u8	status;
	u8	version;
	u8	rsvd0[6];

	u16	func_idx;
	u8	cmdq_type;
	u8	rsvd1[1];

	u8	rsvd2[4];

	struct hinic_cmdq_ctxt_info ctxt_info;
};

struct hinic_cmdq {
	struct hinic_wq		*wq;

	enum hinic_cmdq_type	cmdq_type;
	int			wrapped;

	/* Lock for keeping the doorbell order */
	spinlock_t		cmdq_lock;

	struct completion	**done;
	int			**errcode;

	/* doorbell area */
	void __iomem		*db_base;
};

struct hinic_cmdqs {
	struct hinic_hwif	*hwif;

	struct pci_pool		*cmdq_buf_pool;

	struct hinic_wq		*saved_wqs;

	struct hinic_cmdq_pages cmdq_pages;

	struct hinic_cmdq	cmdq[HINIC_MAX_CMDQ_TYPES];
};

int hinic_alloc_cmdq_buf(struct hinic_cmdqs *cmdqs,
			 struct hinic_cmdq_buf *cmdq_buf);

void hinic_free_cmdq_buf(struct hinic_cmdqs *cmdqs,
			 struct hinic_cmdq_buf *cmdq_buf);

int hinic_cmdq_direct_resp(struct hinic_cmdqs *cmdqs,
			   enum hinic_mod_type mod, u8 cmd,
			   struct hinic_cmdq_buf *buf_in, u64 *out_param);

int hinic_init_cmdqs(struct hinic_cmdqs *cmdqs, struct hinic_hwif *hwif,
		     void __iomem **db_area);

void hinic_free_cmdqs(struct hinic_cmdqs *cmdqs);

#endif
