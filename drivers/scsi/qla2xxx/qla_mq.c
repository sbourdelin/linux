/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2016 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"
#include "qla_gbl.h"

static int
qla2xxx_set_affinity_hint(struct qla_qpair *qpair, cpumask_var_t cpu_mask)
{
	int ret;

	if (!qpair || !qpair->msix)
		return -EINVAL;

	ret = irq_set_affinity_hint(qpair->msix->vector, cpu_mask);

	return ret;
}

void
qla2xxx_qpair_sp_free_dma(void *vha, void *ptr)
{
	srb_t *sp = (srb_t *)ptr;
	struct scsi_cmnd *cmd = GET_CMD_SP(sp);
	struct qla_hw_data *ha = sp->fcport->vha->hw;
	void *ctx = GET_CMD_CTX_SP(sp);

	if (sp->flags & SRB_DMA_VALID) {
		scsi_dma_unmap(cmd);
		sp->flags &= ~SRB_DMA_VALID;
	}

	if (sp->flags & SRB_CRC_PROT_DMA_VALID) {
		dma_unmap_sg(&ha->pdev->dev, scsi_prot_sglist(cmd),
		    scsi_prot_sg_count(cmd), cmd->sc_data_direction);
		sp->flags &= ~SRB_CRC_PROT_DMA_VALID;
	}

	if (sp->flags & SRB_CRC_CTX_DSD_VALID) {
		/* List assured to be having elements */
		qla2x00_clean_dsd_pool(ha, sp, NULL);
		sp->flags &= ~SRB_CRC_CTX_DSD_VALID;
	}

	if (sp->flags & SRB_CRC_CTX_DMA_VALID) {
		dma_pool_free(ha->dl_dma_pool, ctx,
		    ((struct crc_context *)ctx)->crc_ctx_dma);
		sp->flags &= ~SRB_CRC_CTX_DMA_VALID;
	}

	if (sp->flags & SRB_FCP_CMND_DMA_VALID) {
		struct ct6_dsd *ctx1 = (struct ct6_dsd *)ctx;

		dma_pool_free(ha->fcp_cmnd_dma_pool, ctx1->fcp_cmnd,
		    ctx1->fcp_cmnd_dma);
		list_splice(&ctx1->dsd_list, &ha->gbl_dsd_list);
		ha->gbl_dsd_inuse -= ctx1->dsd_use_cnt;
		ha->gbl_dsd_avail += ctx1->dsd_use_cnt;
		mempool_free(ctx1, ha->ctx_mempool);
	}

	CMD_SP(cmd) = NULL;
	qla2xxx_rel_qpair_sp(sp->qpair, sp);
}

void
qla2xxx_qpair_sp_compl(void *data, void *ptr, int res)
{
	srb_t *sp = (srb_t *)ptr;
	struct scsi_cmnd *cmd = GET_CMD_SP(sp);

	cmd->result = res;

	if (atomic_read(&sp->ref_count) == 0) {
		ql_dbg(ql_dbg_io, sp->fcport->vha, 0x3079,
		    "SP reference-count to ZERO -- sp=%p cmd=%p.\n",
		    sp, GET_CMD_SP(sp));
		if (ql2xextended_error_logging & ql_dbg_io)
			WARN_ON(atomic_read(&sp->ref_count) == 0);
		return;
	}
	if (!atomic_dec_and_test(&sp->ref_count))
		return;

	qla2xxx_qpair_sp_free_dma(sp->fcport->vha, sp);
	cmd->scsi_done(cmd);
}

struct qla_qpair *qla2xxx_create_qpair(struct scsi_qla_host *vha, cpumask_var_t cpu_mask, int qos, int vp_idx)
{
	int rsp_id = 0;
	int  req_id = 0;
	int i;
	int cpu_id;
	struct qla_hw_data *ha = vha->hw;
	uint16_t qpair_id = 0;
	struct qla_qpair *qpair = NULL;
	struct qla_msix_entry *msix;
	struct qla_percpu_qp_hint *hint;

	if (!(ha->fw_attributes & BIT_6) || !ha->flags.msix_enabled) {
		ql_log(ql_log_warn, vha, 0x00181,
		    "FW/Driver is not multi-queue capable.\n");
		return NULL;
	}
	if (ql2xmqsupport) {
		qpair = kzalloc(sizeof(struct qla_qpair), GFP_KERNEL);
		if (qpair == NULL) {
			ql_log(ql_log_warn, vha, 0x0182,
			    "Failed to allocate memory for queue pair.\n");
			return NULL;
		}
		memset(qpair, 0, sizeof(struct qla_qpair));

		qpair->hw = vha->hw;

		/* Assign available que pair id */
		mutex_lock(&ha->mq_lock);
		qpair_id = find_first_zero_bit(ha->qpair_qid_map, ha->max_qpairs);
		if (qpair_id >= ha->max_qpairs) {
			mutex_unlock(&ha->mq_lock);
			ql_log(ql_log_warn, vha, 0x0183,
			    "No resources to create additional q pair.\n");
			goto fail_qid_map;
		}
		set_bit(qpair_id, ha->qpair_qid_map);
		ha->queue_pair_map[qpair_id] = qpair;
		qpair->id = qpair_id;
		qpair->vp_idx = vp_idx;

		for (i = 0; i < ha->msix_count; i++) {
			msix = &ha->msix_entries[i];
			if (msix->in_use)
				continue;
			qpair->msix = msix;
			ql_log(ql_dbg_multiq, vha, 0xc00f,
			    "Vector %x selected for qpair\n", msix->vector);
			break;
		}
		if (!qpair->msix) {
			ql_log(ql_log_warn, vha, 0x0184,
			    "Out of MSI-X vectors!.\n");
			goto fail_msix;
		}

		qpair->msix->in_use = 1;
		list_add_tail(&qpair->qp_list_elem, &vha->qp_list);

		mutex_unlock(&ha->mq_lock);

		/* Create response queue first */
		rsp_id = qla25xx_create_rsp_que(ha, 0, 0, 0, qpair);
		if (!rsp_id) {
			ql_log(ql_log_warn, vha, 0x0185,
			    "Failed to create response queue.\n");
			goto fail_rsp;
		}

		qpair->rsp = ha->rsp_q_map[rsp_id];

		/* Create request queue */
		req_id = qla25xx_create_req_que(ha, 0, vp_idx, 0, rsp_id, qos);
		if (!req_id) {
			ql_log(ql_log_warn, vha, 0x0186,
			    "Failed to create request queue.\n");
			goto fail_req;
		}

		qpair->req = ha->req_q_map[req_id];
		qpair->rsp->req = qpair->req;

		if (IS_T10_PI_CAPABLE(ha) && ql2xenabledif) {
			if (ha->fw_attributes & BIT_4)
				qpair->difdix_supported = 1;
		}

		qpair->srb_mempool = mempool_create_slab_pool(SRB_MIN_REQ, srb_cachep);
		if (!qpair->srb_mempool) {
			ql_log(ql_log_warn, vha, 0x0191,
			    "Failed to create srb mempool for qpair %d\n",
			    qpair->id);
			goto fail_mempool;
		}

		/* Set CPU affinity hint */
		if (cpu_mask)
			qla2xxx_set_affinity_hint(qpair, cpu_mask);

		if (cpu_mask) {
			cpumask_copy(&qpair->cpu_mask, cpu_mask);
			for_each_cpu(cpu_id, cpu_mask) {
				hint = per_cpu_ptr(vha->qps_hint, cpu_id);
				hint->change_in_progress = 1;
				hint->qp = qpair;
				hint->change_in_progress = 0;
			}
		}

		/* Mark as online */
		qpair->online = 1;

		if (!vha->flags.qpairs_available)
			vha->flags.qpairs_available = 1;

		ql_dbg(ql_dbg_multiq, vha, 0xc00d,
		    "Request/Response queue pair created, id %d\n",
		    qpair->id);
		ql_dbg(ql_dbg_init, vha, 0x0187,
		    "Request/Response queue pair created, id %d\n",
		    qpair->id);
	}
	return qpair;

fail_mempool:
fail_req:
	qla25xx_delete_rsp_que(vha, qpair->rsp);
fail_rsp:
	mutex_lock(&ha->mq_lock);
	qpair->msix->in_use = 0;
	list_del(&qpair->qp_list_elem);
	if (list_empty(&vha->qp_list))
		vha->flags.qpairs_available = 0;
	if (cpu_mask) {
		for_each_cpu(cpu_id, cpu_mask) {
			hint = per_cpu_ptr(vha->qps_hint, cpu_id);
			hint->change_in_progress = 1;
			hint->qp = NULL;
			hint->change_in_progress = 0;
		}
	}
fail_msix:
	ha->queue_pair_map[qpair_id] = NULL;
	clear_bit(qpair_id, ha->qpair_qid_map);
	mutex_unlock(&ha->mq_lock);
fail_qid_map:
	kfree(qpair);
	return NULL;
}

int qla2xxx_delete_qpair(struct scsi_qla_host *vha, struct qla_qpair *qpair)
{
	int ret, cpu_id;
	struct qla_hw_data *ha = qpair->hw;
	struct qla_percpu_qp_hint *hint;

	qpair->delete_in_progress = 1;
	while (atomic_read(&qpair->ref_count))
		msleep(500);

	ret = qla25xx_delete_req_que(vha, qpair->req);
	if (ret != QLA_SUCCESS)
		goto fail;
	ret = qla25xx_delete_rsp_que(vha, qpair->rsp);
	if (ret != QLA_SUCCESS)
		goto fail;

	mutex_lock(&ha->mq_lock);
	ha->queue_pair_map[qpair->id] = NULL;
	clear_bit(qpair->id, ha->qpair_qid_map);
	list_del(&qpair->qp_list_elem);
	for_each_cpu(cpu_id, &qpair->cpu_mask) {
		hint = per_cpu_ptr(vha->qps_hint, cpu_id);
		hint->change_in_progress = 1;
		hint->qp = NULL;
		hint->change_in_progress = 0;
	}
	if (list_empty(&vha->qp_list))
		vha->flags.qpairs_available = 0;
	mempool_destroy(qpair->srb_mempool);
	kfree(qpair);
	mutex_unlock(&ha->mq_lock);

	return QLA_SUCCESS;
fail:
	return ret;
}
