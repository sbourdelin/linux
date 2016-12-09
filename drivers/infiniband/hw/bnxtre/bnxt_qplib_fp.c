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

#include "bnxt_re_hsi.h"

#include "bnxt_qplib_res.h"
#include "bnxt_qplib_rcfw.h"
#include "bnxt_qplib_sp.h"
#include "bnxt_qplib_fp.h"

static void bnxt_qplib_arm_cq_enable(struct bnxt_qplib_cq *cq);

static void bnxt_qplib_free_qp_hdr_buf(struct bnxt_qplib_res *res,
				       struct bnxt_qplib_qp *qp)
{
	struct bnxt_qplib_q *rq = &qp->rq;
	struct bnxt_qplib_q *sq = &qp->sq;

	if (qp->rq_hdr_buf)
		dma_free_coherent(&res->pdev->dev,
				  rq->hwq.max_elements * qp->rq_hdr_buf_size,
				  qp->rq_hdr_buf, qp->rq_hdr_buf_map);
	if (qp->sq_hdr_buf)
		dma_free_coherent(&res->pdev->dev,
				  sq->hwq.max_elements * qp->sq_hdr_buf_size,
				  qp->sq_hdr_buf, qp->sq_hdr_buf_map);
	qp->rq_hdr_buf = NULL;
	qp->sq_hdr_buf = NULL;
	qp->rq_hdr_buf_map = 0;
	qp->sq_hdr_buf_map = 0;
	qp->sq_hdr_buf_size = 0;
	qp->rq_hdr_buf_size = 0;
}

static int bnxt_qplib_alloc_qp_hdr_buf(struct bnxt_qplib_res *res,
				       struct bnxt_qplib_qp *qp)
{
	struct bnxt_qplib_q *rq = &qp->rq;
	struct bnxt_qplib_q *sq = &qp->rq;
	int rc = 0;

	if (qp->sq_hdr_buf_size && sq->hwq.max_elements) {
		qp->sq_hdr_buf = dma_alloc_coherent(&res->pdev->dev,
					sq->hwq.max_elements *
					qp->sq_hdr_buf_size,
					&qp->sq_hdr_buf_map, GFP_KERNEL);
		if (!qp->sq_hdr_buf) {
			rc = -ENOMEM;
			dev_err(&res->pdev->dev,
				"QPLIB: Failed to create sq_hdr_buf");
			goto fail;
		}
	}

	if (qp->rq_hdr_buf_size && rq->hwq.max_elements) {
		qp->rq_hdr_buf = dma_alloc_coherent(&res->pdev->dev,
						    rq->hwq.max_elements *
						    qp->rq_hdr_buf_size,
						    &qp->rq_hdr_buf_map,
						    GFP_KERNEL);
		if (!qp->rq_hdr_buf) {
			rc = -ENOMEM;
			dev_err(&res->pdev->dev,
				"QPLIB: Failed to create rq_hdr_buf");
			goto fail;
		}
	}
	return 0;

fail:
	bnxt_qplib_free_qp_hdr_buf(res, qp);
	return rc;
}

static void bnxt_qplib_service_nq(unsigned long data)
{
	struct bnxt_qplib_nq *nq = (struct bnxt_qplib_nq *)data;
	struct bnxt_qplib_hwq *hwq = &nq->hwq;
	struct nq_base *nqe, **nq_ptr;
	int num_cqne_processed = 0;
	u32 sw_cons, raw_cons;
	u32 type;
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

		type = le16_to_cpu(nqe->info10_type & NQ_BASE_TYPE_MASK);
		switch (type) {
		case NQ_BASE_TYPE_CQ_NOTIFICATION:
		{
			struct nq_cn *nqcne = (struct nq_cn *)nqe;

			q_handle = le32_to_cpu(nqcne->cq_handle_low);
			q_handle |= (u64)le32_to_cpu(nqcne->cq_handle_high)
						     << 32;
			bnxt_qplib_arm_cq_enable((struct bnxt_qplib_cq *)
						 q_handle);
			if (!nq->cqn_handler(nq, (struct bnxt_qplib_cq *)
						 q_handle))
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

/* QP */
int bnxt_qplib_create_qp1(struct bnxt_qplib_res *res, struct bnxt_qplib_qp *qp)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct cmdq_create_qp1 req;
	struct creq_create_qp1_resp *resp;
	struct bnxt_qplib_pbl *pbl;
	struct bnxt_qplib_q *sq = &qp->sq;
	struct bnxt_qplib_q *rq = &qp->rq;
	int rc;
	u16 cmd_flags = 0;
	u32 qp_flags = 0;

	RCFW_CMD_PREP(req, CREATE_QP1, cmd_flags);

	/* General */
	req.type = qp->type;
	req.dpi = cpu_to_le32(qp->dpi->dpi);
	req.qp_handle = cpu_to_le64(qp->qp_handle);

	/* SQ */
	sq->hwq.max_elements = sq->max_wqe;
	rc = bnxt_qplib_alloc_init_hwq(res->pdev, &sq->hwq, NULL, 0,
				       &sq->hwq.max_elements,
				       BNXT_QPLIB_MAX_SQE_ENTRY_SIZE, 0,
				       PAGE_SIZE, HWQ_TYPE_QUEUE);
	if (rc)
		goto exit;

	sq->swq = kcalloc(sq->hwq.max_elements, sizeof(*sq->swq), GFP_KERNEL);
	if (!sq->swq) {
		rc = -ENOMEM;
		goto fail_sq;
	}
	pbl = &sq->hwq.pbl[PBL_LVL_0];
	req.sq_pbl = cpu_to_le64(pbl->pg_map_arr[0]);
	req.sq_pg_size_sq_lvl =
		((sq->hwq.level & CMDQ_CREATE_QP1_SQ_LVL_MASK)
				<<  CMDQ_CREATE_QP1_SQ_LVL_SFT) |
		(pbl->pg_size == ROCE_PG_SIZE_4K ?
				CMDQ_CREATE_QP1_SQ_PG_SIZE_PG_4K :
		 pbl->pg_size == ROCE_PG_SIZE_8K ?
				CMDQ_CREATE_QP1_SQ_PG_SIZE_PG_8K :
		 pbl->pg_size == ROCE_PG_SIZE_64K ?
				CMDQ_CREATE_QP1_SQ_PG_SIZE_PG_64K :
		 pbl->pg_size == ROCE_PG_SIZE_2M ?
				CMDQ_CREATE_QP1_SQ_PG_SIZE_PG_2M :
		 pbl->pg_size == ROCE_PG_SIZE_8M ?
				CMDQ_CREATE_QP1_SQ_PG_SIZE_PG_8M :
		 pbl->pg_size == ROCE_PG_SIZE_1G ?
				CMDQ_CREATE_QP1_SQ_PG_SIZE_PG_1G :
		 CMDQ_CREATE_QP1_SQ_PG_SIZE_PG_4K);

	if (qp->scq)
		req.scq_cid = cpu_to_le32(qp->scq->id);

	qp_flags |= CMDQ_CREATE_QP1_QP_FLAGS_RESERVED_LKEY_ENABLE;

	/* RQ */
	if (rq->max_wqe) {
		rq->hwq.max_elements = qp->rq.max_wqe;
		rc = bnxt_qplib_alloc_init_hwq(res->pdev, &rq->hwq, NULL, 0,
					       &rq->hwq.max_elements,
					       BNXT_QPLIB_MAX_RQE_ENTRY_SIZE, 0,
					       PAGE_SIZE, HWQ_TYPE_QUEUE);
		if (rc)
			goto fail_sq;

		rq->swq = kcalloc(rq->hwq.max_elements, sizeof(*rq->swq),
				  GFP_KERNEL);
		if (!rq->swq) {
			rc = -ENOMEM;
			goto fail_rq;
		}
		pbl = &rq->hwq.pbl[PBL_LVL_0];
		req.rq_pbl = cpu_to_le64(pbl->pg_map_arr[0]);
		req.rq_pg_size_rq_lvl =
			((rq->hwq.level & CMDQ_CREATE_QP1_RQ_LVL_MASK) <<
			 CMDQ_CREATE_QP1_RQ_LVL_SFT) |
				(pbl->pg_size == ROCE_PG_SIZE_4K ?
					CMDQ_CREATE_QP1_RQ_PG_SIZE_PG_4K :
				 pbl->pg_size == ROCE_PG_SIZE_8K ?
					CMDQ_CREATE_QP1_RQ_PG_SIZE_PG_8K :
				 pbl->pg_size == ROCE_PG_SIZE_64K ?
					CMDQ_CREATE_QP1_RQ_PG_SIZE_PG_64K :
				 pbl->pg_size == ROCE_PG_SIZE_2M ?
					CMDQ_CREATE_QP1_RQ_PG_SIZE_PG_2M :
				 pbl->pg_size == ROCE_PG_SIZE_8M ?
					CMDQ_CREATE_QP1_RQ_PG_SIZE_PG_8M :
				 pbl->pg_size == ROCE_PG_SIZE_1G ?
					CMDQ_CREATE_QP1_RQ_PG_SIZE_PG_1G :
				 CMDQ_CREATE_QP1_RQ_PG_SIZE_PG_4K);
		if (qp->rcq)
			req.rcq_cid = cpu_to_le32(qp->rcq->id);
	}

	/* Header buffer - allow hdr_buf pass in */
	rc = bnxt_qplib_alloc_qp_hdr_buf(res, qp);
	if (rc) {
		rc = -ENOMEM;
		goto fail;
	}
	req.qp_flags = cpu_to_le32(qp_flags);
	req.sq_size = cpu_to_le32(sq->hwq.max_elements);
	req.rq_size = cpu_to_le32(rq->hwq.max_elements);

	req.sq_fwo_sq_sge =
		cpu_to_le16((sq->max_sge & CMDQ_CREATE_QP1_SQ_SGE_MASK) <<
			    CMDQ_CREATE_QP1_SQ_SGE_SFT);
	req.rq_fwo_rq_sge =
		cpu_to_le16((rq->max_sge & CMDQ_CREATE_QP1_RQ_SGE_MASK) <<
			    CMDQ_CREATE_QP1_RQ_SGE_SFT);

	req.pd_id = cpu_to_le32(qp->pd->id);

	resp = (struct creq_create_qp1_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 0);
	if (!resp) {
		dev_err(&res->pdev->dev, "QPLIB: FP: CREATE_QP1 send failed");
		rc = -EINVAL;
		goto fail;
	}
	/**/
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_QP1 timed out");
		rc = -ETIMEDOUT;
		goto fail;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_QP1 failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
		rc = -EINVAL;
		goto fail;
	}
	qp->id = le32_to_cpu(resp->xid);
	qp->cur_qp_state = CMDQ_MODIFY_QP_NEW_STATE_RESET;
	sq->flush_in_progress = false;
	rq->flush_in_progress = false;

	return 0;

fail:
	bnxt_qplib_free_qp_hdr_buf(res, qp);
fail_rq:
	bnxt_qplib_free_hwq(res->pdev, &rq->hwq);
	kfree(rq->swq);
fail_sq:
	bnxt_qplib_free_hwq(res->pdev, &sq->hwq);
	kfree(sq->swq);
exit:
	return rc;
}

int bnxt_qplib_create_qp(struct bnxt_qplib_res *res, struct bnxt_qplib_qp *qp)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct sq_send *hw_sq_send_hdr, **hw_sq_send_ptr;
	struct cmdq_create_qp req;
	struct creq_create_qp_resp *resp;
	struct bnxt_qplib_pbl *pbl;
	struct sq_psn_search **psn_search_ptr;
	unsigned long long int psn_search, poff = 0;
	struct bnxt_qplib_q *sq = &qp->sq;
	struct bnxt_qplib_q *rq = &qp->rq;
	struct bnxt_qplib_hwq *xrrq;
	int i, rc, req_size, psn_sz;
	u16 cmd_flags = 0, max_ssge;
	u32 sw_prod, qp_flags = 0;

	RCFW_CMD_PREP(req, CREATE_QP, cmd_flags);

	/* General */
	req.type = qp->type;
	req.dpi = cpu_to_le32(qp->dpi->dpi);
	req.qp_handle = cpu_to_le64(qp->qp_handle);

	/* SQ */
	psn_sz = (qp->type == CMDQ_CREATE_QP_TYPE_RC) ?
		 sizeof(struct sq_psn_search) : 0;
	sq->hwq.max_elements = sq->max_wqe;
	rc = bnxt_qplib_alloc_init_hwq(res->pdev, &sq->hwq, sq->sglist,
				       sq->nmap, &sq->hwq.max_elements,
				       BNXT_QPLIB_MAX_SQE_ENTRY_SIZE,
				       psn_sz,
				       PAGE_SIZE, HWQ_TYPE_QUEUE);
	if (rc)
		goto exit;

	sq->swq = kcalloc(sq->hwq.max_elements, sizeof(*sq->swq), GFP_KERNEL);
	if (!sq->swq) {
		rc = -ENOMEM;
		goto fail_sq;
	}
	hw_sq_send_ptr = (struct sq_send **)sq->hwq.pbl_ptr;
	if (psn_sz) {
		psn_search_ptr = (struct sq_psn_search **)
				  &hw_sq_send_ptr[SQE_PG(sq->hwq.max_elements)];
		psn_search = (unsigned long long int)
			      &hw_sq_send_ptr[SQE_PG(sq->hwq.max_elements)]
			      [SQE_IDX(sq->hwq.max_elements)];
		if (psn_search & ~PAGE_MASK) {
			/* If the psn_search does not start on a page boundary,
			 * then calculate the offset
			 */
			poff = (psn_search & ~PAGE_MASK) /
				BNXT_QPLIB_MAX_PSNE_ENTRY_SIZE;
		}
		for (i = 0; i < sq->hwq.max_elements; i++)
			sq->swq[i].psn_search =
				&psn_search_ptr[PSNE_PG(i + poff)]
					       [PSNE_IDX(i + poff)];
	}
	pbl = &sq->hwq.pbl[PBL_LVL_0];
	req.sq_pbl = cpu_to_le64(pbl->pg_map_arr[0]);
	req.sq_pg_size_sq_lvl =
		((sq->hwq.level & CMDQ_CREATE_QP_SQ_LVL_MASK)
				 <<  CMDQ_CREATE_QP_SQ_LVL_SFT) |
		(pbl->pg_size == ROCE_PG_SIZE_4K ?
				CMDQ_CREATE_QP_SQ_PG_SIZE_PG_4K :
		 pbl->pg_size == ROCE_PG_SIZE_8K ?
				CMDQ_CREATE_QP_SQ_PG_SIZE_PG_8K :
		 pbl->pg_size == ROCE_PG_SIZE_64K ?
				CMDQ_CREATE_QP_SQ_PG_SIZE_PG_64K :
		 pbl->pg_size == ROCE_PG_SIZE_2M ?
				CMDQ_CREATE_QP_SQ_PG_SIZE_PG_2M :
		 pbl->pg_size == ROCE_PG_SIZE_8M ?
				CMDQ_CREATE_QP_SQ_PG_SIZE_PG_8M :
		 pbl->pg_size == ROCE_PG_SIZE_1G ?
				CMDQ_CREATE_QP_SQ_PG_SIZE_PG_1G :
		 CMDQ_CREATE_QP_SQ_PG_SIZE_PG_4K);

	/* initialize all SQ WQEs to LOCAL_INVALID (sq prep for hw fetch) */
	hw_sq_send_ptr = (struct sq_send **)sq->hwq.pbl_ptr;
	for (sw_prod = 0; sw_prod < sq->hwq.max_elements; sw_prod++) {
		hw_sq_send_hdr = &hw_sq_send_ptr[SQE_PG(sw_prod)]
						[SQE_IDX(sw_prod)];
		hw_sq_send_hdr->wqe_type = SQ_BASE_WQE_TYPE_LOCAL_INVALID;
	}

	if (qp->scq)
		req.scq_cid = cpu_to_le32(qp->scq->id);

	qp_flags |= CMDQ_CREATE_QP_QP_FLAGS_RESERVED_LKEY_ENABLE;
	qp_flags |= CMDQ_CREATE_QP_QP_FLAGS_FR_PMR_ENABLED;
	if (qp->sig_type)
		qp_flags |= CMDQ_CREATE_QP_QP_FLAGS_FORCE_COMPLETION;

	/* RQ */
	if (rq->max_wqe) {
		rq->hwq.max_elements = rq->max_wqe;
		rc = bnxt_qplib_alloc_init_hwq(res->pdev, &rq->hwq, rq->sglist,
					       rq->nmap, &rq->hwq.max_elements,
					       BNXT_QPLIB_MAX_RQE_ENTRY_SIZE, 0,
					       PAGE_SIZE, HWQ_TYPE_QUEUE);
		if (rc)
			goto fail_sq;

		rq->swq = kcalloc(rq->hwq.max_elements, sizeof(*rq->swq),
				  GFP_KERNEL);
		if (!rq->swq) {
			rc = -ENOMEM;
			goto fail_rq;
		}
		pbl = &rq->hwq.pbl[PBL_LVL_0];
		req.rq_pbl = cpu_to_le64(pbl->pg_map_arr[0]);
		req.rq_pg_size_rq_lvl =
			((rq->hwq.level & CMDQ_CREATE_QP_RQ_LVL_MASK) <<
			 CMDQ_CREATE_QP_RQ_LVL_SFT) |
				(pbl->pg_size == ROCE_PG_SIZE_4K ?
					CMDQ_CREATE_QP_RQ_PG_SIZE_PG_4K :
				 pbl->pg_size == ROCE_PG_SIZE_8K ?
					CMDQ_CREATE_QP_RQ_PG_SIZE_PG_8K :
				 pbl->pg_size == ROCE_PG_SIZE_64K ?
					CMDQ_CREATE_QP_RQ_PG_SIZE_PG_64K :
				 pbl->pg_size == ROCE_PG_SIZE_2M ?
					CMDQ_CREATE_QP_RQ_PG_SIZE_PG_2M :
				 pbl->pg_size == ROCE_PG_SIZE_8M ?
					CMDQ_CREATE_QP_RQ_PG_SIZE_PG_8M :
				 pbl->pg_size == ROCE_PG_SIZE_1G ?
					CMDQ_CREATE_QP_RQ_PG_SIZE_PG_1G :
				 CMDQ_CREATE_QP_RQ_PG_SIZE_PG_4K);
	}

	if (qp->rcq)
		req.rcq_cid = cpu_to_le32(qp->rcq->id);
	req.qp_flags = cpu_to_le32(qp_flags);
	req.sq_size = cpu_to_le32(sq->hwq.max_elements);
	req.rq_size = cpu_to_le32(rq->hwq.max_elements);
	qp->sq_hdr_buf = NULL;
	qp->rq_hdr_buf = NULL;

	rc = bnxt_qplib_alloc_qp_hdr_buf(res, qp);
	if (rc)
		goto fail_rq;

	/* CTRL-22434: Irrespective of the requested SGE count on the SQ
	 * always create the QP with max send sges possible if the requested
	 * inline size is greater than 0.
	 */
	max_ssge = qp->max_inline_data ? 6 : sq->max_sge;
	req.sq_fwo_sq_sge = cpu_to_le16(
				((max_ssge & CMDQ_CREATE_QP_SQ_SGE_MASK)
				 << CMDQ_CREATE_QP_SQ_SGE_SFT) | 0);
	req.rq_fwo_rq_sge = cpu_to_le16(
				((rq->max_sge & CMDQ_CREATE_QP_RQ_SGE_MASK)
				 << CMDQ_CREATE_QP_RQ_SGE_SFT) | 0);
	/* ORRQ and IRRQ */
	if (psn_sz) {
		xrrq = &qp->orrq;
		xrrq->max_elements =
			ORD_LIMIT_TO_ORRQ_SLOTS(qp->max_rd_atomic);
		req_size = xrrq->max_elements *
			   BNXT_QPLIB_MAX_ORRQE_ENTRY_SIZE + PAGE_SIZE - 1;
		req_size &= ~(PAGE_SIZE - 1);
		rc = bnxt_qplib_alloc_init_hwq(res->pdev, xrrq, NULL, 0,
					       &xrrq->max_elements,
					       BNXT_QPLIB_MAX_ORRQE_ENTRY_SIZE,
					       0, req_size, HWQ_TYPE_CTX);
		if (rc)
			goto fail_buf_free;
		pbl = &xrrq->pbl[PBL_LVL_0];
		req.orrq_addr = cpu_to_le64(pbl->pg_map_arr[0]);

		xrrq = &qp->irrq;
		xrrq->max_elements = IRD_LIMIT_TO_IRRQ_SLOTS(
						qp->max_dest_rd_atomic);
		req_size = xrrq->max_elements *
			   BNXT_QPLIB_MAX_IRRQE_ENTRY_SIZE + PAGE_SIZE - 1;
		req_size &= ~(PAGE_SIZE - 1);

		rc = bnxt_qplib_alloc_init_hwq(res->pdev, xrrq, NULL, 0,
					       &xrrq->max_elements,
					       BNXT_QPLIB_MAX_IRRQE_ENTRY_SIZE,
					       0, req_size, HWQ_TYPE_CTX);
		if (rc)
			goto fail_orrq;

		pbl = &xrrq->pbl[PBL_LVL_0];
		req.irrq_addr = cpu_to_le64(pbl->pg_map_arr[0]);
	}
	req.pd_id = cpu_to_le32(qp->pd->id);

	resp = (struct creq_create_qp_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 0);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_QP send failed");
		rc = -EINVAL;
		goto fail;
	}
	/**/
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_QP timed out");
		rc = -ETIMEDOUT;
		goto fail;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_QP failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
		rc = -EINVAL;
		goto fail;
	}
	qp->id = le32_to_cpu(resp->xid);
	qp->cur_qp_state = CMDQ_MODIFY_QP_NEW_STATE_RESET;
	sq->flush_in_progress = false;
	rq->flush_in_progress = false;

	return 0;

fail:
	if (qp->irrq.max_elements)
		bnxt_qplib_free_hwq(res->pdev, &qp->irrq);
fail_orrq:
	if (qp->orrq.max_elements)
		bnxt_qplib_free_hwq(res->pdev, &qp->orrq);
fail_buf_free:
	bnxt_qplib_free_qp_hdr_buf(res, qp);
fail_rq:
	bnxt_qplib_free_hwq(res->pdev, &rq->hwq);
	kfree(rq->swq);
fail_sq:
	bnxt_qplib_free_hwq(res->pdev, &sq->hwq);
	kfree(sq->swq);
exit:
	return rc;
}

static void __filter_modify_flags(struct bnxt_qplib_qp *qp)
{
	switch (qp->cur_qp_state) {
	case CMDQ_MODIFY_QP_NEW_STATE_RESET:
		switch (qp->state) {
		case CMDQ_MODIFY_QP_NEW_STATE_INIT:
			break;
		default:
			break;
		}
		break;
	case CMDQ_MODIFY_QP_NEW_STATE_INIT:
		switch (qp->state) {
		case CMDQ_MODIFY_QP_NEW_STATE_RTR:
			/* INIT->RTR, configure the path_mtu to the default
			 * 2048 if not being requested
			 */
			if (!(qp->modify_flags &
			      CMDQ_MODIFY_QP_MODIFY_MASK_PATH_MTU)) {
				qp->modify_flags |=
					CMDQ_MODIFY_QP_MODIFY_MASK_PATH_MTU;
				qp->path_mtu = CMDQ_MODIFY_QP_PATH_MTU_MTU_2048;
			}
			qp->modify_flags &=
				~CMDQ_MODIFY_QP_MODIFY_MASK_VLAN_ID;
			/* Bono FW requires the max_dest_rd_atomic to be >= 1 */
			if (qp->max_dest_rd_atomic < 1)
				qp->max_dest_rd_atomic = 1;
			qp->modify_flags &= ~CMDQ_MODIFY_QP_MODIFY_MASK_SRC_MAC;
			/* Bono FW 20.6.5 requires SGID_INDEX configuration */
			if (!(qp->modify_flags &
			      CMDQ_MODIFY_QP_MODIFY_MASK_SGID_INDEX)) {
				qp->modify_flags |=
					CMDQ_MODIFY_QP_MODIFY_MASK_SGID_INDEX;
				qp->ah.sgid_index = 0;
			}
			break;
		default:
			break;
		}
		break;
	case CMDQ_MODIFY_QP_NEW_STATE_RTR:
		switch (qp->state) {
		case CMDQ_MODIFY_QP_NEW_STATE_RTS:
			/* Bono FW requires the max_rd_atomic to be >= 1 */
			if (qp->max_rd_atomic < 1)
				qp->max_rd_atomic = 1;
			/* Bono FW does not allow PKEY_INDEX,
			 * DGID, FLOW_LABEL, SGID_INDEX, HOP_LIMIT,
			 * TRAFFIC_CLASS, DEST_MAC, PATH_MTU, RQ_PSN,
			 * MIN_RNR_TIMER, MAX_DEST_RD_ATOMIC, DEST_QP_ID
			 * modification
			 */
			qp->modify_flags &=
				~(CMDQ_MODIFY_QP_MODIFY_MASK_PKEY |
				  CMDQ_MODIFY_QP_MODIFY_MASK_DGID |
				  CMDQ_MODIFY_QP_MODIFY_MASK_FLOW_LABEL |
				  CMDQ_MODIFY_QP_MODIFY_MASK_SGID_INDEX |
				  CMDQ_MODIFY_QP_MODIFY_MASK_HOP_LIMIT |
				  CMDQ_MODIFY_QP_MODIFY_MASK_TRAFFIC_CLASS |
				  CMDQ_MODIFY_QP_MODIFY_MASK_DEST_MAC |
				  CMDQ_MODIFY_QP_MODIFY_MASK_PATH_MTU |
				  CMDQ_MODIFY_QP_MODIFY_MASK_RQ_PSN |
				  CMDQ_MODIFY_QP_MODIFY_MASK_MIN_RNR_TIMER |
				  CMDQ_MODIFY_QP_MODIFY_MASK_MAX_DEST_RD_ATOMIC
				  | CMDQ_MODIFY_QP_MODIFY_MASK_DEST_QP_ID);
			break;
		default:
			break;
		}
		break;
	case CMDQ_MODIFY_QP_NEW_STATE_RTS:
		break;
	case CMDQ_MODIFY_QP_NEW_STATE_SQD:
		break;
	case CMDQ_MODIFY_QP_NEW_STATE_SQE:
		break;
	case CMDQ_MODIFY_QP_NEW_STATE_ERR:
		break;
	default:
		break;
	}
}

int bnxt_qplib_modify_qp(struct bnxt_qplib_res *res, struct bnxt_qplib_qp *qp)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct cmdq_modify_qp req;
	struct creq_modify_qp_resp *resp;
	u16 cmd_flags = 0, pkey;
	u32 temp32[4];
	u32 bmask;

	RCFW_CMD_PREP(req, MODIFY_QP, cmd_flags);

	/* Filter out the qp_attr_mask based on the state->new transition */
	__filter_modify_flags(qp);
	bmask = qp->modify_flags;
	req.modify_mask = cpu_to_le64(qp->modify_flags);
	req.qp_cid = cpu_to_le32(qp->id);
	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_STATE) {
		req.network_type_en_sqd_async_notify_new_state =
				(qp->state & CMDQ_MODIFY_QP_NEW_STATE_MASK) |
				(qp->en_sqd_async_notify ?
					CMDQ_MODIFY_QP_EN_SQD_ASYNC_NOTIFY : 0);
	}
	req.network_type_en_sqd_async_notify_new_state |= qp->nw_type;

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_ACCESS)
		req.access = qp->access;

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_PKEY) {
		if (!bnxt_qplib_get_pkey(res, &res->pkey_tbl,
					 qp->pkey_index, &pkey))
			req.pkey = cpu_to_le16(pkey);
	}
	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_QKEY)
		req.qkey = cpu_to_le32(qp->qkey);

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_DGID) {
		memcpy(temp32, qp->ah.dgid.data, sizeof(struct bnxt_qplib_gid));
		req.dgid[0] = cpu_to_le32(temp32[0]);
		req.dgid[1] = cpu_to_le32(temp32[1]);
		req.dgid[2] = cpu_to_le32(temp32[2]);
		req.dgid[3] = cpu_to_le32(temp32[3]);
	}
	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_FLOW_LABEL)
		req.flow_label = cpu_to_le32(qp->ah.flow_label);

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_SGID_INDEX)
		req.sgid_index = cpu_to_le16(res->sgid_tbl.hw_id
					     [qp->ah.sgid_index]);

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_HOP_LIMIT)
		req.hop_limit = qp->ah.hop_limit;

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_TRAFFIC_CLASS)
		req.traffic_class = qp->ah.traffic_class;

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_DEST_MAC)
		memcpy(req.dest_mac, qp->ah.dmac, 6);

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_PATH_MTU)
		req.path_mtu = cpu_to_le16(qp->path_mtu);

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_TIMEOUT)
		req.timeout = qp->timeout;

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_RETRY_CNT)
		req.retry_cnt = qp->retry_cnt;

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_RNR_RETRY)
		req.rnr_retry = qp->rnr_retry;

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_MIN_RNR_TIMER)
		req.min_rnr_timer = qp->min_rnr_timer;

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_RQ_PSN)
		req.rq_psn = cpu_to_le32(qp->rq.psn);

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_SQ_PSN)
		req.sq_psn = cpu_to_le32(qp->sq.psn);

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_MAX_RD_ATOMIC)
		req.max_rd_atomic =
			ORD_LIMIT_TO_ORRQ_SLOTS(qp->max_rd_atomic);

	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_MAX_DEST_RD_ATOMIC)
		req.max_dest_rd_atomic =
			IRD_LIMIT_TO_IRRQ_SLOTS(qp->max_dest_rd_atomic);

	req.sq_size = cpu_to_le32(qp->sq.hwq.max_elements);
	req.rq_size = cpu_to_le32(qp->rq.hwq.max_elements);
	req.sq_sge = cpu_to_le16(qp->sq.max_sge);
	req.rq_sge = cpu_to_le16(qp->rq.max_sge);
	req.max_inline_data = cpu_to_le32(qp->max_inline_data);
	if (bmask & CMDQ_MODIFY_QP_MODIFY_MASK_DEST_QP_ID)
		req.dest_qp_id = cpu_to_le32(qp->dest_qpn);

	req.vlan_pcp_vlan_dei_vlan_id = cpu_to_le16(qp->vlan_id);

	resp = (struct creq_modify_qp_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 0);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: MODIFY_QP send failed");
		return -EINVAL;
	}
	/**/
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: MODIFY_QP timed out");
		return -ETIMEDOUT;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: MODIFY_QP failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
		return -EINVAL;
	}
	qp->cur_qp_state = qp->state;
	return 0;
}

int bnxt_qplib_query_qp(struct bnxt_qplib_res *res, struct bnxt_qplib_qp *qp)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct cmdq_query_qp req;
	struct creq_query_qp_resp *resp;
	struct creq_query_qp_resp_sb *sb;
	u16 cmd_flags = 0;
	u32 temp32[4];
	int i;

	RCFW_CMD_PREP(req, QUERY_QP, cmd_flags);

	req.qp_cid = cpu_to_le32(qp->id);
	req.resp_size = sizeof(*sb) / BNXT_QPLIB_CMDQE_UNITS;
	resp = (struct creq_query_qp_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     (void **)&sb, 0);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: QUERY_QP send failed");
		return -EINVAL;
	}
	/**/
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: QUERY_QP timed out");
		return -ETIMEDOUT;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: QUERY_QP failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
		return -EINVAL;
	}
	/* Extract the context from the side buffer */
	qp->state = sb->en_sqd_async_notify_state &
			CREQ_QUERY_QP_RESP_SB_STATE_MASK;
	qp->en_sqd_async_notify = sb->en_sqd_async_notify_state &
				  CREQ_QUERY_QP_RESP_SB_EN_SQD_ASYNC_NOTIFY ?
				  true : false;
	qp->access = sb->access;
	qp->pkey_index = le16_to_cpu(sb->pkey);
	qp->qkey = le32_to_cpu(sb->qkey);

	temp32[0] = le32_to_cpu(sb->dgid[0]);
	temp32[1] = le32_to_cpu(sb->dgid[1]);
	temp32[2] = le32_to_cpu(sb->dgid[2]);
	temp32[3] = le32_to_cpu(sb->dgid[3]);
	memcpy(qp->ah.dgid.data, temp32, sizeof(qp->ah.dgid.data));

	qp->ah.flow_label = le32_to_cpu(sb->flow_label);

	qp->ah.sgid_index = 0;
	for (i = 0; i < res->sgid_tbl.max; i++) {
		if (res->sgid_tbl.hw_id[i] == le16_to_cpu(sb->sgid_index)) {
			qp->ah.sgid_index = i;
			break;
		}
	}
	if (i == res->sgid_tbl.max)
		dev_warn(&res->pdev->dev, "QPLIB: SGID not found??");

	qp->ah.hop_limit = sb->hop_limit;
	qp->ah.traffic_class = sb->traffic_class;
	memcpy(qp->ah.dmac, sb->dest_mac, 6);
	qp->ah.vlan_id = le16_to_cpu((sb->path_mtu_dest_vlan_id &
				CREQ_QUERY_QP_RESP_SB_VLAN_ID_MASK) >>
				CREQ_QUERY_QP_RESP_SB_VLAN_ID_SFT);
	qp->path_mtu = sb->path_mtu_dest_vlan_id &
				    CREQ_QUERY_QP_RESP_SB_PATH_MTU_MASK;
	qp->timeout = sb->timeout;
	qp->retry_cnt = sb->retry_cnt;
	qp->rnr_retry = sb->rnr_retry;
	qp->min_rnr_timer = sb->min_rnr_timer;
	qp->rq.psn = le32_to_cpu(sb->rq_psn);
	qp->max_rd_atomic = ORRQ_SLOTS_TO_ORD_LIMIT(sb->max_rd_atomic);
	qp->sq.psn = le32_to_cpu(sb->sq_psn);
	qp->max_dest_rd_atomic =
			IRRQ_SLOTS_TO_IRD_LIMIT(sb->max_dest_rd_atomic);
	qp->sq.max_wqe = qp->sq.hwq.max_elements;
	qp->rq.max_wqe = qp->rq.hwq.max_elements;
	qp->sq.max_sge = le16_to_cpu(sb->sq_sge);
	qp->rq.max_sge = le32_to_cpu(sb->rq_sge);
	qp->max_inline_data = le32_to_cpu(sb->max_inline_data);
	qp->dest_qpn = le32_to_cpu(sb->dest_qp_id);
	memcpy(qp->smac, sb->src_mac, 6);
	qp->vlan_id = le16_to_cpu(sb->vlan_pcp_vlan_dei_vlan_id);
	return 0;
}

static void __clean_cq(struct bnxt_qplib_cq *cq, u64 qp)
{
	struct bnxt_qplib_hwq *cq_hwq = &cq->hwq;
	struct cq_base *hw_cqe, **hw_cqe_ptr;
	int i;

	for (i = 0; i < cq_hwq->max_elements; i++) {
		hw_cqe_ptr = (struct cq_base **)cq_hwq->pbl_ptr;
		hw_cqe = &hw_cqe_ptr[CQE_PG(i)][CQE_IDX(i)];
		if (!CQE_CMP_VALID(hw_cqe, i, cq_hwq->max_elements))
			continue;
		switch (hw_cqe->cqe_type_toggle & CQ_BASE_CQE_TYPE_MASK) {
		case CQ_BASE_CQE_TYPE_REQ:
		case CQ_BASE_CQE_TYPE_TERMINAL:
		{
			struct cq_req *cqe = (struct cq_req *)hw_cqe;

			if (qp == le64_to_cpu(cqe->qp_handle))
				cqe->qp_handle = 0;
			break;
		}
		case CQ_BASE_CQE_TYPE_RES_RC:
		case CQ_BASE_CQE_TYPE_RES_UD:
		case CQ_BASE_CQE_TYPE_RES_RAWETH_QP1:
		{
			struct cq_res_rc *cqe = (struct cq_res_rc *)hw_cqe;

			if (qp == le64_to_cpu(cqe->qp_handle))
				cqe->qp_handle = 0;
			break;
		}
		default:
			break;
		}
	}
}

static unsigned long bnxt_qplib_lock_cqs(struct bnxt_qplib_qp *qp)
{
	unsigned long flags;

	spin_lock_irqsave(&qp->scq->hwq.lock, flags);
	if (qp->rcq && qp->rcq != qp->scq)
		spin_lock(&qp->rcq->hwq.lock);

	return flags;
}

static void bnxt_qplib_unlock_cqs(struct bnxt_qplib_qp *qp,
				  unsigned long flags)
{
	if (qp->rcq && qp->rcq != qp->scq)
		spin_unlock(&qp->rcq->hwq.lock);
	spin_unlock_irqrestore(&qp->scq->hwq.lock, flags);
}

int bnxt_qplib_destroy_qp(struct bnxt_qplib_res *res,
			  struct bnxt_qplib_qp *qp)
{
	struct bnxt_qplib_rcfw *rcfw = res->rcfw;
	struct cmdq_destroy_qp req;
	struct creq_destroy_qp_resp *resp;
	unsigned long flags;
	u16 cmd_flags = 0;

	RCFW_CMD_PREP(req, DESTROY_QP, cmd_flags);

	req.qp_cid = cpu_to_le32(qp->id);
	resp = (struct creq_destroy_qp_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 0);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: DESTROY_QP send failed");
		return -EINVAL;
	}
	/**/
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: DESTROY_QP timed out");
		return -ETIMEDOUT;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: DESTROY_QP failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
		return -EINVAL;
	}

	/* Must walk the associated CQs to nullified the QP ptr */
	flags = bnxt_qplib_lock_cqs(qp);
	__clean_cq(qp->scq, (u64)qp);
	if (qp->rcq != qp->scq)
		__clean_cq(qp->rcq, (u64)qp);
	bnxt_qplib_unlock_cqs(qp, flags);

	bnxt_qplib_free_qp_hdr_buf(res, qp);
	bnxt_qplib_free_hwq(res->pdev, &qp->sq.hwq);
	kfree(qp->sq.swq);

	bnxt_qplib_free_hwq(res->pdev, &qp->rq.hwq);
	kfree(qp->rq.swq);

	if (qp->irrq.max_elements)
		bnxt_qplib_free_hwq(res->pdev, &qp->irrq);
	if (qp->orrq.max_elements)
		bnxt_qplib_free_hwq(res->pdev, &qp->orrq);

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

	req.cq_fco_cnq_id = cpu_to_le16(
			((cq->cnq_hw_ring_id & CMDQ_CREATE_CQ_CNQ_ID_MASK) <<
			 CMDQ_CREATE_CQ_CNQ_ID_SFT) | 0);

	resp = (struct creq_create_cq_resp *)
			bnxt_qplib_rcfw_send_message(rcfw, (void *)&req,
						     NULL, 0);
	if (!resp) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_CQ send failed");
		return -EINVAL;
	}
	/**/
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_CQ timed out");
		rc = -ETIMEDOUT;
		goto fail;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: CREATE_CQ failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
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
	/**/
	if (!bnxt_qplib_rcfw_wait_for_resp(rcfw, le16_to_cpu(req.cookie))) {
		/* Cmd timed out */
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: DESTROY_CQ timed out");
		return -ETIMEDOUT;
	}
	if (RCFW_RESP_STATUS(resp) ||
	    RCFW_RESP_COOKIE(resp) != RCFW_CMDQ_COOKIE(req)) {
		dev_err(&rcfw->pdev->dev, "QPLIB: FP: DESTROY_CQ failed ");
		dev_err(&rcfw->pdev->dev,
			"QPLIB: with status 0x%x cmdq 0x%x resp 0x%x",
			RCFW_RESP_STATUS(resp), RCFW_CMDQ_COOKIE(req),
			RCFW_RESP_COOKIE(resp));
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
