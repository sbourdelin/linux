/*
 * Copyright (c) 2016 Hisilicon Limited.
 *
 * Authors: Wei Hu <xavier.huwei@huawei.com>
 * Authors: Nenglong Zhao <zhaonenglong@hisilicon.com>
 * Authors: Lijun Ou <oulijun@huawei.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include "hns_roce_common.h"
#include "hns_roce_device.h"
#include "hns_roce_hw_v1.h"

/**
 * hns_roce_v1_reset - reset roce
 * @hr_dev: roce device struct pointer
 * @val: 1 -- drop reset, 0 -- reset
 * return 0 - success , negative --fail
 */
int hns_roce_v1_reset(struct hns_roce_dev *hr_dev, u32 val)
{
	struct device_node *dsaf_node;
	struct device *dev = &hr_dev->pdev->dev;
	struct device_node *np = dev->of_node;
	int ret;

	dsaf_node = of_parse_phandle(np, "dsaf-handle", 0);

	if (!val) {
		ret = hns_dsaf_roce_reset(&dsaf_node->fwnode, 0);
	} else {
		ret = hns_dsaf_roce_reset(&dsaf_node->fwnode, 0);
		if (ret)
			return ret;

		msleep(SLEEP_TIME_INTERVAL);
		ret = hns_dsaf_roce_reset(&dsaf_node->fwnode, 1);
	}

		return ret;
}

void hns_roce_v1_profile(struct hns_roce_dev *hr_dev)
{
	int i = 0;
	struct hns_roce_caps *caps = &hr_dev->caps;

	hr_dev->vendor_id = le32_to_cpu(roce_readl((hr_dev->reg_base +
			    ROCEE_VENDOR_ID_REG)));
	hr_dev->vendor_part_id = le32_to_cpu(roce_readl((hr_dev->reg_base +
				 ROCEE_VENDOR_PART_ID_REG)));
	hr_dev->hw_rev = le32_to_cpu(roce_readl((hr_dev->reg_base +
			 ROCEE_HW_VERSION_REG)));
	hr_dev->fw_ver = 0;

	hr_dev->sys_image_guid = le32_to_cpu(roce_readl(hr_dev->reg_base +
					     ROCEE_SYS_IMAGE_GUID_L_REG)) |
				((u64)le32_to_cpu(roce_readl(hr_dev->reg_base +
					     ROCEE_SYS_IMAGE_GUID_H_REG)) <<
					     ADDR_SHIFT_32);

	caps->fw_ver		= hr_dev->hw_rev;
	caps->num_qps		= HNS_ROCE_V1_MAX_QP_NUM;
	caps->max_wqes		= HNS_ROCE_V1_MAX_WQE_NUM;
	caps->num_cqs		= HNS_ROCE_V1_MAX_CQ_NUM;
	caps->max_cqes		= HNS_ROCE_V1_MAX_CQE_NUM;
	caps->max_sq_sg		= HNS_ROCE_V1_SG_NUM;
	caps->max_rq_sg		= HNS_ROCE_V1_SG_NUM;
	caps->max_sq_inline	= HNS_ROCE_V1_INLINE_SIZE;
	caps->num_uars		= HNS_ROCE_V1_UAR_NUM;
	caps->phy_num_uars	= HNS_ROCE_V1_PHY_UAR_NUM;
	caps->num_aeq_vectors	= HNS_ROCE_AEQE_VEC_NUM;
	caps->num_comp_vectors	= HNS_ROCE_COMP_VEC_NUM;
	caps->num_other_vectors	= HNS_ROCE_AEQE_OF_VEC_NUM;
	caps->num_mtpts		= HNS_ROCE_V1_MAX_MTPT_NUM;
	caps->num_mtt_segs	= HNS_ROCE_V1_MAX_MTT_SEGS;
	caps->num_pds		= HNS_ROCE_V1_MAX_PD_NUM;
	caps->max_qp_init_rdma	= HNS_ROCE_V1_MAX_QP_INIT_RDMA;
	caps->max_qp_dest_rdma	= HNS_ROCE_V1_MAX_QP_DEST_RDMA;
	caps->max_sq_desc_sz	= HNS_ROCE_V1_MAX_SQ_DESC_SZ;
	caps->max_rq_desc_sz	= HNS_ROCE_V1_MAX_RQ_DESC_SZ;
	caps->qpc_entry_sz	= HNS_ROCE_V1_QPC_ENTRY_SIZE;
	caps->irrl_entry_sz	= HNS_ROCE_V1_IRRL_ENTRY_SIZE;
	caps->cqc_entry_sz	= HNS_ROCE_V1_CQC_ENTRY_SIZE;
	caps->mtpt_entry_sz	= HNS_ROCE_V1_MTPT_ENTRY_SIZE;
	caps->mtt_entry_sz	= HNS_ROCE_V1_MTT_ENTRY_SIZE;
	caps->cq_entry_sz	= HNS_ROCE_V1_CQE_ENTRY_SIZE;
	caps->page_size_cap	= HNS_ROCE_V1_PAGE_SIZE_SUPPORT;
	caps->sqp_start		= 0;
	caps->reserved_lkey	= 0;
	caps->reserved_pds	= 0;
	caps->reserved_mrws	= 1;
	caps->reserved_mtts	= 0;
	caps->reserved_uars	= 0;
	caps->reserved_cqs	= 0;

	for (i = 0; i < caps->num_ports; i++)
		caps->pkey_table_len[i] = 1;

	for (i = 0; i < caps->num_ports; i++) {
		/* Six ports shared 16 GID in v1 engine */
		if (i >= (HNS_ROCE_V1_GID_NUM % caps->num_ports))
			caps->gid_table_len[i] = HNS_ROCE_V1_GID_NUM /
						 caps->num_ports;
		else
			caps->gid_table_len[i] = HNS_ROCE_V1_GID_NUM /
						 caps->num_ports + 1;
	}

	for (i = 0; i < caps->num_comp_vectors; i++)
		caps->ceqe_depth[i] = HNS_ROCE_V1_NUM_COMP_EQE;

	caps->aeqe_depth = HNS_ROCE_V1_NUM_ASYNC_EQE;
	caps->local_ca_ack_delay = le32_to_cpu(roce_readl((hr_dev->reg_base +
				   ROCEE_ACK_DELAY_REG)));
	caps->max_mtu = IB_MTU_2048;
}

struct hns_roce_hw hns_roce_hw_v1 = {
	.reset = hns_roce_v1_reset,
	.hw_profile = hns_roce_v1_profile,
};
