/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef _HNS_ROCE_DEVICE_H
#define _HNS_ROCE_DEVICE_H

#include <linux/platform_device.h>
#include <linux/radix-tree.h>
#include <linux/semaphore.h>
#include <rdma/ib_addr.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_umem.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>

#define DRV_NAME "hns_roce"

#define HNS_ROCE_MAX_IRQ_NUM			34
#define HNS_ROCE_MAX_PORTS			6

#define HNS_ROCE_COMP_VEC_NUM			32

#define HNS_ROCE_AEQE_VEC_NUM			1
#define HNS_ROCE_AEQE_OF_VEC_NUM		1

#define ADDR_SHIFT_32				32

struct hns_roce_ib_iboe {
	struct net_device      *netdevs[HNS_ROCE_MAX_PORTS];
	u8			phy_port[HNS_ROCE_MAX_PORTS];
};

struct hns_roce_caps {
	u64		fw_ver;
	u8		num_ports;
	int		gid_table_len[HNS_ROCE_MAX_PORTS];
	int		pkey_table_len[HNS_ROCE_MAX_PORTS];
	int		local_ca_ack_delay;
	int		num_uars;
	u32		phy_num_uars;
	u32		max_sq_sg;	/* 2 */
	u32		max_sq_inline;	/* 32 */
	u32		max_rq_sg;	/* 2 */
	int		num_qps;	/* 256k */
	u32		max_wqes;	/* 16k */
	u32		max_sq_desc_sz;	/* 64 */
	u32		max_rq_desc_sz;	/* 64 */
	int		max_qp_init_rdma;
	int		max_qp_dest_rdma;
	int		sqp_start;
	int		num_cqs;
	int		max_cqes;
	int		reserved_cqs;
	int		num_aeq_vectors;	/* 1 */
	int		num_comp_vectors;	/* 32 ceq */
	int		num_other_vectors;
	int		num_mtpts;
	u32		num_mtt_segs;
	int		reserved_mtts;
	int		reserved_mrws;
	int		reserved_uars;
	int		num_pds;
	int		reserved_pds;
	u32		mtt_entry_sz;
	u32		cq_entry_sz;
	u32		page_size_cap;
	u32		reserved_lkey;
	int		mtpt_entry_sz;
	int		qpc_entry_sz;
	int		irrl_entry_sz;
	int		cqc_entry_sz;
	int		aeqe_depth;
	int		ceqe_depth[HNS_ROCE_COMP_VEC_NUM];
	enum ib_mtu	max_mtu;
};

struct hns_roce_hw {
	int (*reset)(struct hns_roce_dev *hr_dev, u32 val);
	void (*hw_profile)(struct hns_roce_dev *hr_dev);
};

struct hns_roce_dev {
	struct ib_device	ib_dev;
	struct platform_device  *pdev;
	struct hns_roce_ib_iboe iboe;

	int			irq[HNS_ROCE_MAX_IRQ_NUM];
	u8 __iomem		*reg_base;
	struct hns_roce_caps	caps;

	u64                     fw_ver;
	u64			sys_image_guid;
	u32                     vendor_id;
	u32                     vendor_part_id;
	u32                     hw_rev;

	int			cmd_mod;
	int			loop_idc;
	struct hns_roce_hw	*hw;
};

extern struct hns_roce_hw hns_roce_hw_v1;

#endif /* _HNS_ROCE_DEVICE_H */
