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
 * Description: Fast Path Operators
 */

#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/prefetch.h>

#include "roce_hsi.h"

#include "qplib_res.h"
#include "qplib_rcfw.h"
#include "qplib_sp.h"
#include "qplib_fp.h"

static void bnxt_qplib_arm_cq_enable(struct bnxt_qplib_cq *cq);
static void bnxt_qplib_service_nq(unsigned long data)
{
	struct bnxt_qplib_nq *nq = (struct bnxt_qplib_nq *)data;
	struct bnxt_qplib_hwq *hwq = &nq->hwq;
	struct nq_base *nqe, **nq_ptr;
	int num_cqne_processed = 0;
	u32 sw_cons, raw_cons;
	u16 type;
	int budget = nq->budget;
	u64 q_handle;

	/* Service the NQ until empty */
	raw_cons = hwq->cons;
	while (budget--) {
		sw_cons = HWQ_CMP(raw_cons, hwq);
		nq_ptr = (struct nq_base **)hwq->pbl_ptr;
		nqe = &nq_ptr[NQE_PG(sw_cons)][NQE_IDX(sw_cons)];
		if (!NQE_CMP_VALID(nqe, raw_cons, hwq->max_elements))
			break;

		type = le16_to_cpu(nqe->info10_type) & NQ_BASE_TYPE_MASK;
		switch (type) {
		case NQ_BASE_TYPE_CQ_NOTIFICATION:
		{
			struct nq_cn *nqcne = (struct nq_cn *)nqe;

			q_handle = le32_to_cpu(nqcne->cq_handle_low);
			q_handle |= (u64)le32_to_cpu(nqcne->cq_handle_high)
						     << 32;
			bnxt_qplib_arm_cq_enable((struct bnxt_qplib_cq *)
						 ((unsigned long)q_handle));
			if (!nq->cqn_handler(nq, (struct bnxt_qplib_cq *)
						 ((unsigned long)q_handle)))
				num_cqne_processed++;
			else
				dev_warn(&nq->pdev->dev,
					 "QPLIB: cqn - type 0x%x not handled",
					 type);
			break;
		}
		case NQ_BASE_TYPE_DBQ_EVENT:
			break;
		default:
			dev_warn(&nq->pdev->dev,
				 "QPLIB: nqe with type = 0x%x not handled",
				 type);
			break;
		}
		raw_cons++;
	}
	if (hwq->cons != raw_cons) {
		hwq->cons = raw_cons;
		NQ_DB_REARM(nq->bar_reg_iomem, hwq->cons, hwq->max_elements);
	}
}

static irqreturn_t bnxt_qplib_nq_irq(int irq, void *dev_instance)
{
	struct bnxt_qplib_nq *nq = dev_instance;
	struct bnxt_qplib_hwq *hwq = &nq->hwq;
	struct nq_base **nq_ptr;
	u32 sw_cons;

	/* Prefetch the NQ element */
	sw_cons = HWQ_CMP(hwq->cons, hwq);
	nq_ptr = (struct nq_base **)nq->hwq.pbl_ptr;
	prefetch(&nq_ptr[NQE_PG(sw_cons)][NQE_IDX(sw_cons)]);

	/* Fan out to CPU affinitized kthreads? */
	tasklet_schedule(&nq->worker);

	return IRQ_HANDLED;
}

void bnxt_qplib_disable_nq(struct bnxt_qplib_nq *nq)
{
	/* Make sure the HW is stopped! */
	synchronize_irq(nq->vector);
	tasklet_disable(&nq->worker);
	tasklet_kill(&nq->worker);

	if (nq->requested) {
		free_irq(nq->vector, nq);
		nq->requested = false;
	}
	if (nq->bar_reg_iomem)
		iounmap(nq->bar_reg_iomem);
	nq->bar_reg_iomem = NULL;

	nq->cqn_handler = NULL;
	nq->srqn_handler = NULL;
	nq->vector = 0;
}

int bnxt_qplib_enable_nq(struct pci_dev *pdev, struct bnxt_qplib_nq *nq,
			 int msix_vector, int bar_reg_offset,
			 int (*cqn_handler)(struct bnxt_qplib_nq *nq,
					    void *),
			 int (*srqn_handler)(struct bnxt_qplib_nq *nq,
					     void *, u8 event))
{
	resource_size_t nq_base;
	int rc;

	nq->pdev = pdev;
	nq->vector = msix_vector;

	nq->cqn_handler = cqn_handler;

	nq->srqn_handler = srqn_handler;

	tasklet_init(&nq->worker, bnxt_qplib_service_nq, (unsigned long)nq);

	nq->requested = false;
	rc = request_irq(nq->vector, bnxt_qplib_nq_irq, 0, "bnxt_qplib_nq", nq);
	if (rc) {
		dev_err(&nq->pdev->dev,
			"Failed to request IRQ for NQ: %#x", rc);
		bnxt_qplib_disable_nq(nq);
		goto fail;
	}
	nq->requested = true;
	nq->bar_reg = NQ_CONS_PCI_BAR_REGION;
	nq->bar_reg_off = bar_reg_offset;
	nq_base = pci_resource_start(pdev, nq->bar_reg);
	if (!nq_base) {
		rc = -ENOMEM;
		goto fail;
	}
	nq->bar_reg_iomem = ioremap_nocache(nq_base + nq->bar_reg_off, 4);
	if (!nq->bar_reg_iomem) {
		rc = -ENOMEM;
		goto fail;
	}
	NQ_DB_REARM(nq->bar_reg_iomem, nq->hwq.cons, nq->hwq.max_elements);

	return 0;
fail:
	bnxt_qplib_disable_nq(nq);
	return rc;
}

void bnxt_qplib_free_nq(struct bnxt_qplib_nq *nq)
{
	if (nq->hwq.max_elements)
		bnxt_qplib_free_hwq(nq->pdev, &nq->hwq);
}

int bnxt_qplib_alloc_nq(struct pci_dev *pdev, struct bnxt_qplib_nq *nq)
{
	nq->pdev = pdev;
	if (!nq->hwq.max_elements ||
	    nq->hwq.max_elements > BNXT_QPLIB_NQE_MAX_CNT)
		nq->hwq.max_elements = BNXT_QPLIB_NQE_MAX_CNT;

	if (bnxt_qplib_alloc_init_hwq(nq->pdev, &nq->hwq, NULL, 0,
				      &nq->hwq.max_elements,
				      BNXT_QPLIB_MAX_NQE_ENTRY_SIZE, 0,
				      PAGE_SIZE, HWQ_TYPE_L2_CMPL))
		return -ENOMEM;

	nq->budget = 8;
	return 0;
}

/* CQ */

/* Spinlock must be held */
static void bnxt_qplib_arm_cq_enable(struct bnxt_qplib_cq *cq)
{
	struct dbr_dbr db_msg = { 0 };

	db_msg.type_xid =
		cpu_to_le32(((cq->id << DBR_DBR_XID_SFT) & DBR_DBR_XID_MASK) |
			    DBR_DBR_TYPE_CQ_ARMENA);
	/* Flush memory writes before enabling the CQ */
	wmb();
	__iowrite64_copy(cq->dbr_base, &db_msg, sizeof(db_msg) / sizeof(u64));
}

static void bnxt_qplib_arm_cq(struct bnxt_qplib_cq *cq, u32 arm_type)
{
	struct bnxt_qplib_hwq *cq_hwq = &cq->hwq;
	struct dbr_dbr db_msg = { 0 };
	u32 sw_cons;

	/* Ring DB */
	sw_cons = HWQ_CMP(cq_hwq->cons, cq_hwq);
	db_msg.index = cpu_to_le32((sw_cons << DBR_DBR_INDEX_SFT) &
				    DBR_DBR_INDEX_MASK);
	db_msg.type_xid =
		cpu_to_le32(((cq->id << DBR_DBR_XID_SFT) & DBR_DBR_XID_MASK) |
			    arm_type);
	/* flush memory writes before arming the CQ */
	wmb();
	__iowrite64_copy(cq->dpi->dbr, &db_msg, sizeof(db_msg) / sizeof(u64));
}

int bnxt_qplib_create_cq(struct bnxt_qplib_res *res, struct bnxt_qplib_cq *cq)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct cmdq_create_cq req;
	struct creq_create_cq_resp *resp;
	struct bnxt_qplib_pbl *pbl;
	u16 cmd_flags = 0;
	int rc;

	cq->hwq.max_elements = cq->max_wqe;
	rc = bnxt_qplib_alloc_init_hwq(res->pdev, &cq->hwq, cq->sghead,
				       cq->nmap, &cq->hwq.max_elements,
				       BNXT_QPLIB_MAX_CQE_ENTRY_SIZE, 0,
				       PAGE_SIZE, HWQ_TYPE_QUEUE);
	if (rc)
		goto exit;

	RCFW_CMD_PREP(req, CREATE_CQ, cmd_flags);

	if (!cq->dpi) {
		dev_err(&rcfw->pdev->dev,
			"QPLIB: FP: CREATE_CQ failed due to NULL DPI");
		return -EINVAL;
	}
	req.dpi = cpu_to_le32(cq->dpi->dpi);
	req.cq_handle = cpu_to_le64(cq->cq_handle);

	req.cq_size = cpu_to_le32(cq->hwq.max_elements);
	pbl = &cq->hwq.pbl[PBL_LVL_0];
	req.pg_size_lvl = cpu_to_le32(
	    ((cq->hwq.level & CMDQ_CREATE_CQ_LVL_MASK) <<
						CMDQ_CREATE_CQ_LVL_SFT) |
	    (pbl->pg_size == ROCE_PG_SIZE_4K ? CMDQ_CREATE_CQ_PG_SIZE_PG_4K :
	     pbl->pg_size == ROCE_PG_SIZE_8K ? CMDQ_CREATE_CQ_PG_SIZE_PG_8K :
	     pbl->pg_size == ROCE_PG_SIZE_64K ? CMDQ_CREATE_CQ_PG_SIZE_PG_64K :
	     pbl->pg_size == ROCE_PG_SIZE_2M ? CMDQ_CREATE_CQ_PG_SIZE_PG_2M :
	     pbl->pg_size == ROCE_PG_SIZE_8M ? CMDQ_CREATE_CQ_PG_SIZE_PG_8M :
	     pbl->pg_size == ROCE_PG_SIZE_1G ? CMDQ_CREATE_CQ_PG_SIZE_PG_1G :
	     CMDQ_CREATE_CQ_PG_SIZE_PG_4K));

	req.pbl = cpu_to_le64(pbl->pg_map_arr[0]);

	req.cq_fco_cnq_id = cpu_to_le32(
			(cq->cnq_hw_ring_id & CMDQ_CREATE_CQ_CNQ_ID_MASK) <<
			 CMDQ_CREATE_CQ_CNQ_ID_SFT);

	resp = (struct creq_create_cq_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 0);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_CQ send failed");
		return -EINVAL;
	}
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_CQ timed out");
		rc = -ETIMEDOUT;
		goto fail;
	}
	if (resp->status ||
	    le16_to_cpu(resp->cookie) != le16_to_cpu(req.cookie)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_CQ failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			resp->status, le16_to_cpu(req.cookie),
			le16_to_cpu(resp->cookie));
		rc = -EINVAL;
		goto fail;
	}
	cq->id = le32_to_cpu(resp->xid);
	cq->dbr_base = res->dpi_tbl.dbr_bar_reg_iomem;
	cq->period = BNXT_QPLIB_QUEUE_START_PERIOD;
	init_waitqueue_head(&cq->waitq);

	bnxt_qplib_arm_cq_enable(cq);
	return 0;

fail:
	bnxt_qplib_free_hwq(res->pdev, &cq->hwq);
exit:
	return rc;
}

int bnxt_qplib_destroy_cq(struct bnxt_qplib_res *res, struct bnxt_qplib_cq *cq)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct cmdq_destroy_cq req;
	struct creq_destroy_cq_resp *resp;
	u16 cmd_flags = 0;

	RCFW_CMD_PREP(req, DESTROY_CQ, cmd_flags);

	req.cq_cid = cpu_to_le32(cq->id);
	resp = (struct creq_destroy_cq_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 0);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: DESTROY_CQ send failed");
		return -EINVAL;
	}
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: DESTROY_CQ timed out");
		return -ETIMEDOUT;
	}
	if (resp->status ||
	    le16_to_cpu(resp->cookie) != le16_to_cpu(req.cookie)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: DESTROY_CQ failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			resp->status, le16_to_cpu(req.cookie),
			le16_to_cpu(resp->cookie));
		return -EINVAL;
	}
	bnxt_qplib_free_hwq(res->pdev, &cq->hwq);
	return 0;
}

void bnxt_qplib_req_notify_cq(struct bnxt_qplib_cq *cq, u32 arm_type)
{
	unsigned long flags;

	spin_lock_irqsave(&cq->hwq.lock, flags);
	if (arm_type)
		bnxt_qplib_arm_cq(cq, arm_type);

	spin_unlock_irqrestore(&cq->hwq.lock, flags);
}
