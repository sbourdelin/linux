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
