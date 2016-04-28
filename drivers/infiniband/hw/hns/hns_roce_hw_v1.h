/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _HNS_ROCE_HW_V1_H
#define _HNS_ROCE_HW_V1_H

#define HNS_ROCE_V1_MAX_PD_NUM			0x8000
#define HNS_ROCE_V1_MAX_CQ_NUM			0x10000
#define HNS_ROCE_V1_MAX_CQE_NUM			0x8000

#define HNS_ROCE_V1_MAX_QP_NUM			0x40000
#define HNS_ROCE_V1_MAX_WQE_NUM			0x4000

#define HNS_ROCE_V1_MAX_MTPT_NUM		0x80000

#define HNS_ROCE_V1_MAX_MTT_SEGS		0x100000

#define HNS_ROCE_V1_MAX_QP_INIT_RDMA		128
#define HNS_ROCE_V1_MAX_QP_DEST_RDMA		128

#define HNS_ROCE_V1_MAX_SQ_DESC_SZ		64
#define HNS_ROCE_V1_MAX_RQ_DESC_SZ		64
#define HNS_ROCE_V1_SG_NUM			2
#define HNS_ROCE_V1_INLINE_SIZE			32

#define HNS_ROCE_V1_UAR_NUM			256
#define HNS_ROCE_V1_PHY_UAR_NUM			8

#define HNS_ROCE_V1_GID_NUM			16

#define HNS_ROCE_V1_NUM_COMP_EQE		0x8000
#define	HNS_ROCE_V1_NUM_ASYNC_EQE		0x400

#define HNS_ROCE_V1_QPC_ENTRY_SIZE		256
#define HNS_ROCE_V1_IRRL_ENTRY_SIZE		8
#define HNS_ROCE_V1_CQC_ENTRY_SIZE		64
#define HNS_ROCE_V1_MTPT_ENTRY_SIZE		64
#define HNS_ROCE_V1_MTT_ENTRY_SIZE		64

#define HNS_ROCE_V1_CQE_ENTRY_SIZE		32
#define HNS_ROCE_V1_PAGE_SIZE_SUPPORT		0xFFFFF000

#define SLEEP_TIME_INTERVAL			20

extern int hns_dsaf_roce_reset(struct fwnode_handle *dsaf_fwnode, u32 val);

#endif
