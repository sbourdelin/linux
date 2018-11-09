// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2018-2019 Hisilicon Limited.

#include <linux/device.h>

#include "hclge_cmd.h"
#include "hclge_main.h"
#include "hnae3.h"

static void hclge_print(struct hclge_dev *hdev, bool flag, char *true_buf,
			char *false_buf)
{
	if (flag)
		dev_info(&hdev->pdev->dev, "%s\n", true_buf);
	else
		dev_info(&hdev->pdev->dev, "%s\n", false_buf);
}

static void hclge_dbg_dump_promisc_cfg(struct hclge_dev *hdev, char *cmd_buf)
{
#define HCLGE_DBG_UC_MODE_B BIT(1)
#define HCLGE_DBG_MC_MODE_B BIT(2)
#define HCLGE_DBG_BC_MODE_B BIT(3)

	struct hclge_promisc_cfg_cmd *req;
	struct hclge_desc desc;
	u16 vf_id;
	int ret;

	ret = kstrtou16(&cmd_buf[13], 10, &vf_id);
	if (ret)
		vf_id = 0;

	if (vf_id >= hdev->num_req_vfs) {
		dev_err(&hdev->pdev->dev, "vf_id (%u) is out of range(%u)\n",
			vf_id, hdev->num_req_vfs);
		return;
	}

	hclge_cmd_setup_basic_desc(&desc, HCLGE_OPC_CFG_PROMISC_MODE, true);
	req = (struct hclge_promisc_cfg_cmd *)desc.data;
	req->vf_id = (u8)vf_id;

	ret = hclge_cmd_send(&hdev->hw, &desc, 1);
	if (ret) {
		dev_err(&hdev->pdev->dev,
			"dump promisc mode fail, status is %d.\n", ret);
		return;
	}

	dev_info(&hdev->pdev->dev, "vf(%u) promisc mode\n", req->vf_id);

	hclge_print(hdev, req->flag & HCLGE_DBG_UC_MODE_B,
		    "uc: enable", "uc: disable");
	hclge_print(hdev, req->flag & HCLGE_DBG_MC_MODE_B,
		    "mc: enable", "mc: disable");
	hclge_print(hdev, req->flag & HCLGE_DBG_BC_MODE_B,
		    "bc: enable", "bc: disable");
}

static void hclge_dbg_fd_tcam_read(struct hclge_dev *hdev, u8 stage,
				   bool sel_x, u32 loc)
{
	struct hclge_fd_tcam_config_1_cmd *req1;
	struct hclge_fd_tcam_config_2_cmd *req2;
	struct hclge_fd_tcam_config_3_cmd *req3;
	struct hclge_desc desc[3];
	int ret, i;
	u32 *req;

	hclge_cmd_setup_basic_desc(&desc[0], HCLGE_OPC_FD_TCAM_OP, true);
	desc[0].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
	hclge_cmd_setup_basic_desc(&desc[1], HCLGE_OPC_FD_TCAM_OP, true);
	desc[1].flag |= cpu_to_le16(HCLGE_CMD_FLAG_NEXT);
	hclge_cmd_setup_basic_desc(&desc[2], HCLGE_OPC_FD_TCAM_OP, true);

	req1 = (struct hclge_fd_tcam_config_1_cmd *)desc[0].data;
	req2 = (struct hclge_fd_tcam_config_2_cmd *)desc[1].data;
	req3 = (struct hclge_fd_tcam_config_3_cmd *)desc[2].data;

	req1->stage  = stage;
	req1->xy_sel = sel_x ? 1 : 0;
	req1->index  = cpu_to_le32(loc);

	ret = hclge_cmd_send(&hdev->hw, desc, 3);
	if (ret)
		return;

	dev_info(&hdev->pdev->dev, " read result tcam key %s(%u):\n",
		 sel_x ? "x" : "y", loc);

	req = (u32 *)req1->tcam_data;
	for (i = 0; i < 2; i++)
		dev_info(&hdev->pdev->dev, "%08x\n", *req++);

	req = (u32 *)req2->tcam_data;
	for (i = 0; i < 6; i++)
		dev_info(&hdev->pdev->dev, "%08x\n", *req++);

	req = (u32 *)req3->tcam_data;
	for (i = 0; i < 5; i++)
		dev_info(&hdev->pdev->dev, "%08x\n", *req++);
}

static void hclge_dbg_fd_tcam(struct hclge_dev *hdev)
{
	u32 i;

	for (i = 0; i < hdev->fd_cfg.rule_num[0]; i++) {
		hclge_dbg_fd_tcam_read(hdev, 0, true, i);
		hclge_dbg_fd_tcam_read(hdev, 0, false, i);
	}
}

int hclge_dbg_run_cmd(struct hnae3_handle *handle, char *cmd_buf)
{
	struct hclge_vport *vport = hclge_get_vport(handle);
	struct hclge_dev *hdev = vport->back;

	if (strncmp(cmd_buf, "dump fd tcam", 12) == 0) {
		hclge_dbg_fd_tcam(hdev);
	} else if (strncmp(cmd_buf, "dump promisc", 12) == 0) {
		hclge_dbg_dump_promisc_cfg(hdev, cmd_buf);
	} else {
		dev_info(&hdev->pdev->dev, "unknown command\n");
		return -EINVAL;
	}

	return 0;
}
