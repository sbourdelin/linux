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
 * Description: Slow Path Operators
 */

#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/pci.h>

#include "bnxt_re_hsi.h"

#include "bnxt_qplib_res.h"
#include "bnxt_qplib_rcfw.h"
#include "bnxt_qplib_sp.h"

const struct bnxt_qplib_gid bnxt_qplib_gid_zero = {{ 0, 0, 0, 0, 0, 0, 0, 0,
						     0, 0, 0, 0, 0, 0, 0, 0 } };

/* Device */
int bnxt_qplib_get_dev_attr(struct bnxt_qplib_rcfw *rcfw,
			    struct bnxt_qplib_dev_attr *attr)
{
	struct cmdq_query_func req;
	struct creq_query_func_resp *resp;
	struct creq_query_func_resp_sb *sb;
	u16 cmd_flags = 0;
	u32 temp;
	u8 *tqm_alloc;
	int i;

	RCFW_CMD_PREP(req, QUERY_FUNC, cmd_flags);

	req.resp_size = sizeof(*sb) / BNXT_QPLIB_CMDQE_UNITS;
	resp = (struct creq_query_func_resp *)
		bnxt_qplib_rcfw_send_message(rcfw, (void *)&req, (void **)&sb,
					     0);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: QUERY_FUNC send failed");
		return -EINVAL;
	}
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: QUERY_FUNC timed out");
		return -ETIMEDOUT;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: QUERY_FUNC failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
		return -EINVAL;
	}
	/* Extract the context from the side buffer */
	attr->max_qp = le32_to_cpu(sb->max_qp);
	attr->max_qp_rd_atom =
		sb->max_qp_rd_atom > BNXT_QPLIB_MAX_OUT_RD_ATOM ?
		BNXT_QPLIB_MAX_OUT_RD_ATOM : sb->max_qp_rd_atom;
	attr->max_qp_init_rd_atom =
		sb->max_qp_init_rd_atom > BNXT_QPLIB_MAX_OUT_RD_ATOM ?
		BNXT_QPLIB_MAX_OUT_RD_ATOM : sb->max_qp_init_rd_atom;
	attr->max_qp_wqes = le16_to_cpu(sb->max_qp_wr);
	attr->max_qp_sges = sb->max_sge;
	attr->max_cq = le32_to_cpu(sb->max_cq);
	attr->max_cq_wqes = le32_to_cpu(sb->max_cqe);
	attr->max_cq_sges = attr->max_qp_sges;
	attr->max_mr = le32_to_cpu(sb->max_mr);
	attr->max_mw = le32_to_cpu(sb->max_mw);

	attr->max_mr_size = le64_to_cpu(sb->max_mr_size);
	attr->max_pd = 64 * 1024;
	attr->max_raw_ethy_qp = le32_to_cpu(sb->max_raw_eth_qp);
	attr->max_ah = le32_to_cpu(sb->max_ah);

	attr->max_fmr = le32_to_cpu(sb->max_fmr);
	attr->max_map_per_fmr = le32_to_cpu(sb->max_map_per_fmr);

	attr->max_srq = le16_to_cpu(sb->max_srq);
	attr->max_srq_wqes = le32_to_cpu(sb->max_srq_wr - 1);
	attr->max_srq_sges = sb->max_srq_sge;
	/* Bono only reports 1 PKEY for now, but it can support > 1 */
	attr->max_pkey = le32_to_cpu(sb->max_pkeys);

	attr->max_inline_data = le32_to_cpu(sb->max_inline_data);
	attr->l2_db_size = (sb->l2_db_space_size + 1) * PAGE_SIZE;
	attr->max_sgid = le32_to_cpu(sb->max_gid);

	strlcpy(attr->fw_ver, "20.6.28.0", sizeof(attr->fw_ver));

	for (i = 0; i < MAX_TQM_ALLOC_REQ / 4; i++) {
		temp = le32_to_cpu(sb->tqm_alloc_reqs[i]);
		tqm_alloc = (u8 *)&temp;
		attr->tqm_alloc_reqs[i * 4] = *tqm_alloc;
		attr->tqm_alloc_reqs[i * 4 + 1] = *(++tqm_alloc);
		attr->tqm_alloc_reqs[i * 4 + 2] = *(++tqm_alloc);
		attr->tqm_alloc_reqs[i * 4 + 3] = *(++tqm_alloc);
	}
	return 0;
}

/* SGID */
int bnxt_qplib_get_sgid(struct bnxt_qplib_res *res,
			struct bnxt_qplib_sgid_tbl *sgid_tbl, int index,
			struct bnxt_qplib_gid *gid)
{
	if (index > sgid_tbl->max) {
		dev_err(&res->pdev->dev,
			"QPLIB: Index %d exceeded SGID table max (%d)",
			index, sgid_tbl->max);
		return -EINVAL;
	}
	memcpy(gid, &sgid_tbl->tbl[index], sizeof(*gid));
	return 0;
}

int bnxt_qplib_del_sgid(struct bnxt_qplib_sgid_tbl *sgid_tbl,
			struct bnxt_qplib_gid *gid, bool update)
{
	struct bnxt_qplib_res *res = to_bnxt_qplib(sgid_tbl,
						   struct bnxt_qplib_res,
						   sgid_tbl);
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	int index;

	if (!sgid_tbl) {
		dev_err(&res->pdev->dev, "QPLIB: SGID table not allocated");
		return -EINVAL;
	}
	/* Do we need a sgid_lock here? */
	if (!sgid_tbl->active) {
		dev_err(&res->pdev->dev,
			"QPLIB: SGID table has no active entries");
		return -ENOMEM;
	}
	for (index = 0; index < sgid_tbl->max; index++) {
		if (!memcmp(&sgid_tbl->tbl[index], gid, sizeof(*gid)))
			break;
	}
	if (index == sgid_tbl->max) {
		dev_warn(&res->pdev->dev, "GID not found in the SGID table");
		return 0;
	}
	/* Remove GID from the SGID table */
	if (update) {
		struct cmdq_delete_gid req;
		struct creq_delete_gid_resp *resp;
		u16 cmd_flags = 0;

		RCFW_CMD_PREP(req, DELETE_GID, cmd_flags);
		if (sgid_tbl->hw_id[index] == -1) {
			dev_err(&res->pdev->dev,
				"QPLIB: GID entry contains an invalid HW id");
			return -EINVAL;
		}
		req.gid_index = cpu_to_le16(sgid_tbl->hw_id[index]);
		resp = (struct creq_delete_gid_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req, NULL,
						     0);
		if (!resp) {
			dev_err(&res->pdev->dev,
				"QPLIB: SP: DELETE_GID send failed");
			return -EINVAL;
		}
		if (!bnxt_qplib_rcfw_wait_for_resp(rcfw,
						   le16_to_cpu(req.cookie))) {
			/* Cmd timed out */
			dev_err(&res->pdev->dev,
				"QPLIB: SP: DELETE_GID timed out");
			return -ETIMEDOUT;
		}
		if (RCFW_RESP_STATUS(resp) ||
		    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
			dev_err(&res->pdev->dev,
				"QPLIB: SP: DELETE_GID failed ");
			dev_err(&res->pdev->dev,
				"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
				RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
				RCFW_RESP_COOKIE(resp));
			return -EINVAL;
		}
	}
	memcpy(&sgid_tbl->tbl[index], &bnxt_qplib_gid_zero,
	       sizeof(bnxt_qplib_gid_zero));
	sgid_tbl->active--;
	dev_dbg(&res->pdev->dev,
		"QPLIB: SGID deleted hw_id[0x%x] = 0x%x active = 0x%x",
		 index, sgid_tbl->hw_id[index], sgid_tbl->active);
	sgid_tbl->hw_id[index] = (u16)-1;

	/* unlock */
	return 0;
}

int bnxt_qplib_add_sgid(struct bnxt_qplib_sgid_tbl *sgid_tbl,
			struct bnxt_qplib_gid *gid, u8 *smac, u16 vlan_id,
			bool update, u32 *index)
{
	struct bnxt_qplib_res *res = to_bnxt_qplib(sgid_tbl,
						   struct bnxt_qplib_res,
						   sgid_tbl);
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	int i, free_idx, rc = 0;

	if (!sgid_tbl) {
		dev_err(&res->pdev->dev, "QPLIB: SGID table not allocated");
		return -EINVAL;
	}
	/* Do we need a sgid_lock here? */
	if (sgid_tbl->active == sgid_tbl->max) {
		dev_err(&res->pdev->dev, "QPLIB: SGID table is full");
		return -ENOMEM;
	}
	free_idx = sgid_tbl->max;
	for (i = 0; i < sgid_tbl->max; i++) {
		if (!memcmp(&sgid_tbl->tbl[i], gid, sizeof(*gid))) {
			dev_dbg(&res->pdev->dev,
				"QPLIB: SGID entry already exist in entry %d!",
				i);
			*index = i;
			return -EALREADY;
		} else if (!memcmp(&sgid_tbl->tbl[i], &bnxt_qplib_gid_zero,
				   sizeof(bnxt_qplib_gid_zero)) &&
			   free_idx == sgid_tbl->max) {
			free_idx = i;
		}
	}
	if (free_idx == sgid_tbl->max) {
		dev_err(&res->pdev->dev,
			"QPLIB: SGID table is FULL but count is not MAX??");
		return -ENOMEM;
	}
	if (update) {
		struct cmdq_add_gid req;
		struct creq_add_gid_resp *resp;
		u16 cmd_flags = 0;
		u32 temp32[4];
		u16 temp16[3];

		RCFW_CMD_PREP(req, ADD_GID, cmd_flags);

		memcpy(temp32, gid->data, sizeof(struct bnxt_qplib_gid));
		req.gid[0] = cpu_to_be32(temp32[3]);
		req.gid[1] = cpu_to_be32(temp32[2]);
		req.gid[2] = cpu_to_be32(temp32[1]);
		req.gid[3] = cpu_to_be32(temp32[0]);
		if (vlan_id != 0xFFFF)
			req.vlan = cpu_to_le32((vlan_id &
					CMDQ_ADD_GID_VLAN_VLAN_ID_MASK) |
					CMDQ_ADD_GID_VLAN_TPID_TPID_8100 |
					CMDQ_ADD_GID_VLAN_VLAN_EN);

		/* MAC in network format */
		memcpy(temp16, smac, 6);
		req.src_mac[0] = cpu_to_be16(temp16[0]);
		req.src_mac[1] = cpu_to_be16(temp16[1]);
		req.src_mac[2] = cpu_to_be16(temp16[2]);

		resp = (struct creq_add_gid_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 0);
		if (!resp) {
			dev_err(&res->pdev->dev,
				"QPLIB: SP: ADD_GID send failed");
			return -EINVAL;
		}
		if (!bnxt_qplib_rcfw_wait_for_resp(rcfw,
						   le16_to_cpu(req.cookie))) {
			/* Cmd timed out */
			dev_err(&res->pdev->dev,
				"QPIB: SP: ADD_GID timed out");
			return -ETIMEDOUT;
		}
		if (RCFW_RESP_STATUS(resp) ||
		    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
			dev_err(&res->pdev->dev, "QPLIB: SP: ADD_GID failed ");
			dev_err(&res->pdev->dev,
				"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
				RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
				RCFW_RESP_COOKIE(resp));
			return -EINVAL;
		}
		sgid_tbl->hw_id[free_idx] = le32_to_cpu(resp->xid);
	}
	/* Add GID to the sgid_tbl */
	memcpy(&sgid_tbl->tbl[free_idx], gid, sizeof(*gid));
	sgid_tbl->active++;
	dev_dbg(&res->pdev->dev,
		"QPLIB: SGID added hw_id[0x%x] = 0x%x active = 0x%x",
		 free_idx, sgid_tbl->hw_id[free_idx], sgid_tbl->active);

	*index = free_idx;
	/* unlock */
	return rc;
}

/* pkeys */
int bnxt_qplib_get_pkey(struct bnxt_qplib_res *res,
			struct bnxt_qplib_pkey_tbl *pkey_tbl, u16 index,
			u16 *pkey)
{
	if (index == 0xFFFF) {
		*pkey = 0xFFFF;
		return 0;
	}
	if (index > pkey_tbl->max) {
		dev_err(&res->pdev->dev,
			"QPLIB: Index %d exceeded PKEY table max (%d)",
			index, pkey_tbl->max);
		return -EINVAL;
	}
	memcpy(pkey, &pkey_tbl->tbl[index], sizeof(*pkey));
	return 0;
}

int bnxt_qplib_del_pkey(struct bnxt_qplib_res *res,
			struct bnxt_qplib_pkey_tbl *pkey_tbl, u16 *pkey,
			bool update)
{
	int i, rc = 0;

	if (!pkey_tbl) {
		dev_err(&res->pdev->dev, "QPLIB: PKEY table not allocated");
		return -EINVAL;
	}

	/* Do we need a pkey_lock here? */
	if (!pkey_tbl->active) {
		dev_err(&res->pdev->dev,
			"QPLIB: PKEY table has no active entries");
		return -ENOMEM;
	}
	for (i = 0; i < pkey_tbl->max; i++) {
		if (!memcmp(&pkey_tbl->tbl[i], pkey, sizeof(*pkey)))
			break;
	}
	if (i == pkey_tbl->max) {
		dev_err(&res->pdev->dev,
			"QPLIB: PKEY 0x%04x not found in the pkey table",
			*pkey);
		return -ENOMEM;
	}
	memset(&pkey_tbl->tbl[i], 0, sizeof(*pkey));
	pkey_tbl->active--;

	/* unlock */
	return rc;
}

int bnxt_qplib_add_pkey(struct bnxt_qplib_res *res,
			struct bnxt_qplib_pkey_tbl *pkey_tbl, u16 *pkey,
			bool update)
{
	int i, free_idx, rc = 0;

	if (!pkey_tbl) {
		dev_err(&res->pdev->dev, "QPLIB: PKEY table not allocated");
		return -EINVAL;
	}

	/* Do we need a pkey_lock here? */
	if (pkey_tbl->active == pkey_tbl->max) {
		dev_err(&res->pdev->dev, "QPLIB: PKEY table is full");
		return -ENOMEM;
	}
	free_idx = pkey_tbl->max;
	for (i = 0; i < pkey_tbl->max; i++) {
		if (!memcmp(&pkey_tbl->tbl[i], pkey, sizeof(*pkey)))
			return -EALREADY;
		else if (!pkey_tbl->tbl[i] && free_idx == pkey_tbl->max)
			free_idx = i;
	}
	if (free_idx == pkey_tbl->max) {
		dev_err(&res->pdev->dev,
			"QPLIB: PKEY table is FULL but count is not MAX??");
		return -ENOMEM;
	}
	/* Add PKEY to the pkey_tbl */
	memcpy(&pkey_tbl->tbl[free_idx], pkey, sizeof(*pkey));
	pkey_tbl->active++;

	/* unlock */
	return rc;
}

/* AH */
int bnxt_qplib_create_ah(struct bnxt_qplib_res *res, struct bnxt_qplib_ah *ah)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct cmdq_create_ah req;
	struct creq_create_ah_resp *resp;
	u16 cmd_flags = 0;
	u32 temp32[4];
	u16 temp16[3];

	RCFW_CMD_PREP(req, CREATE_AH, cmd_flags);

	memcpy(temp32, ah->dgid.data, sizeof(struct bnxt_qplib_gid));
	req.dgid[0] = cpu_to_le32(temp32[0]);
	req.dgid[1] = cpu_to_le32(temp32[1]);
	req.dgid[2] = cpu_to_le32(temp32[2]);
	req.dgid[3] = cpu_to_le32(temp32[3]);

	req.type = ah->nw_type;
	req.hop_limit = ah->hop_limit;
	req.sgid_index = cpu_to_le16(res->sgid_tbl.hw_id[ah->sgid_index]);
	req.dest_vlan_id_flow_label = cpu_to_le32((ah->flow_label &
					CMDQ_CREATE_AH_FLOW_LABEL_MASK) |
					CMDQ_CREATE_AH_DEST_VLAN_ID_MASK);
	req.pd_id = cpu_to_le32(ah->pd->id);
	req.traffic_class = ah->traffic_class;

	/* MAC in network format */
	memcpy(temp16, ah->dmac, 6);
	req.dest_mac[0] = cpu_to_le16(temp16[0]);
	req.dest_mac[1] = cpu_to_le16(temp16[1]);
	req.dest_mac[2] = cpu_to_le16(temp16[2]);

	resp = (struct creq_create_ah_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 1);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: CREATE_AH send failed");
		return -EINVAL;
	}
	if (!bnxt_qplib_rcfw_block_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: CREATE_AH timed out");
		return -ETIMEDOUT;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: CREATE_AH failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
		return -EINVAL;
	}
	ah->id = le32_to_cpu(resp->xid);
	return 0;
}

int bnxt_qplib_destroy_ah(struct bnxt_qplib_res *res, struct bnxt_qplib_ah *ah)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct cmdq_destroy_ah req;
	struct creq_destroy_ah_resp *resp;
	u16 cmd_flags = 0;

	/* Clean up the AH table in the device */
	RCFW_CMD_PREP(req, DESTROY_AH, cmd_flags);

	req.ah_cid = cpu_to_le32(ah->id);

	resp = (struct creq_destroy_ah_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 1);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: DESTROY_AH send failed");
		return -EINVAL;
	}
	if (!bnxt_qplib_rcfw_block_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: DESTROY_AH timed out");
		return -ETIMEDOUT;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: SP: DESTROY_AH failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
		return -EINVAL;
	}
	return 0;
}
