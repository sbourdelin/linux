/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2017 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */

#include <linux/scatterlist.h>
#include <linux/delay.h>
#include <linux/nvme.h>
#include <linux/nvme-fc.h>

#include "qla_nvme.h"
#include "qla_nvmet.h"

static void qla_nvmet_send_resp_ctio(struct qla_qpair *qpair,
	struct qla_nvmet_cmd *cmd, struct nvmefc_tgt_fcp_req *rsp);
static void qla_nvmet_send_abts_ctio(struct scsi_qla_host *vha,
		struct abts_recv_from_24xx *abts, bool flag);

/*
 * qla_nvmet_targetport_delete -
 * Invoked by the nvmet to indicate that the target port has
 * been deleted
 */
static void
qla_nvmet_targetport_delete(struct nvmet_fc_target_port *targetport)
{
	struct qla_nvmet_tgtport *tport = targetport->private;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return;

	complete(&tport->tport_del);
}

/*
 * Build NVMET LS response
 */
int
qla_nvmet_ls(srb_t *sp, void *pkt)
{
	struct srb_iocb *nvme;
	struct pt_ls4_request *rsp_pkt = (struct pt_ls4_request *)pkt;
	int     rval = QLA_SUCCESS;

	nvme = &sp->u.iocb_cmd;

	rsp_pkt->entry_type = PT_LS4_REQUEST;
	rsp_pkt->entry_count = 1;
	rsp_pkt->control_flags = cpu_to_le16(CF_LS4_RESPONDER << CF_LS4_SHIFT);
	rsp_pkt->handle = sp->handle;

	rsp_pkt->nport_handle = sp->fcport->loop_id;
	rsp_pkt->vp_index = nvme->u.nvme.vp_index;
	rsp_pkt->exchange_address = cpu_to_le32(nvme->u.nvme.exchange_address);

	rsp_pkt->tx_dseg_count = 1;
	rsp_pkt->tx_byte_count = cpu_to_le16(nvme->u.nvme.rsp_len);
	rsp_pkt->dseg0_len = cpu_to_le16(nvme->u.nvme.rsp_len);
	rsp_pkt->dseg0_address[0] = cpu_to_le32(LSD(nvme->u.nvme.rsp_dma));
	rsp_pkt->dseg0_address[1] = cpu_to_le32(MSD(nvme->u.nvme.rsp_dma));

	ql_log(ql_log_info, sp->vha, 0xffff,
	    "Dumping the NVME-LS response IOCB\n");
	ql_dump_buffer(ql_dbg_disc + ql_dbg_buffer, sp->vha, 0x2075,
	    (uint8_t *)rsp_pkt, sizeof(*rsp_pkt));

	return rval;
}


/*
 * qlt_nvmet_ls_done -
 * Invoked by the firmware interface to indicate the completion
 * of an LS cmd
 * Free all associated resources of the LS cmd
 */
static void qlt_nvmet_ls_done(void *ptr, int res)
{
	struct srb *sp = ptr;
	struct srb_iocb   *nvme = &sp->u.iocb_cmd;
	struct nvmefc_tgt_ls_req *rsp = nvme->u.nvme.desc;
	struct qla_nvmet_cmd *tgt_cmd = nvme->u.nvme.cmd;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return;

	ql_dbg(ql_dbg_nvme, sp->vha, 0x11001,
	    "%s: sp %p vha %p, rsp %p, cmd %p\n", __func__,
	    sp, sp->vha, nvme->u.nvme.desc, nvme->u.nvme.cmd);

	rsp->done(rsp);

	/* Free tgt_cmd */
	kfree(tgt_cmd->buf);
	kfree(tgt_cmd);
	qla2x00_rel_sp(sp);
}

/*
 * qla_nvmet_ls_rsp -
 * Invoked by the nvme-t to complete the LS req.
 * Prepare and send a response CTIO to the firmware.
 */
static int
qla_nvmet_ls_rsp(struct nvmet_fc_target_port *tgtport,
			struct nvmefc_tgt_ls_req *rsp)
{
	struct qla_nvmet_cmd *tgt_cmd =
		container_of(rsp, struct qla_nvmet_cmd, cmd.ls_req);
	struct scsi_qla_host *vha = tgt_cmd->vha;
	struct srb_iocb   *nvme;
	int     rval = QLA_FUNCTION_FAILED;
	srb_t *sp;

	ql_dbg(ql_dbg_nvme + ql_dbg_buffer, vha, 0x11002,
		"Dumping the NVMET-LS response buffer\n");
	ql_dump_buffer(ql_dbg_nvme + ql_dbg_buffer, vha, 0x2075,
		(uint8_t *)rsp->rspbuf, rsp->rsplen);

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, NULL, GFP_ATOMIC);
	if (!sp) {
		ql_log(ql_log_info, vha, 0x11003, "Failed to allocate SRB\n");
		return -ENOMEM;
	}

	sp->type = SRB_NVMET_LS;
	sp->done = qlt_nvmet_ls_done;
	sp->vha = vha;
	sp->fcport = tgt_cmd->fcport;

	nvme = &sp->u.iocb_cmd;
	nvme->u.nvme.rsp_dma = rsp->rspdma;
	nvme->u.nvme.rsp_len = rsp->rsplen;
	nvme->u.nvme.exchange_address = tgt_cmd->atio.u.pt_ls4.exchange_address;
	nvme->u.nvme.nport_handle = tgt_cmd->atio.u.pt_ls4.nport_handle;
	nvme->u.nvme.vp_index = tgt_cmd->atio.u.pt_ls4.vp_index;

	nvme->u.nvme.cmd = tgt_cmd; /* To be freed */
	nvme->u.nvme.desc = rsp; /* Call back to nvmet */

	rval = qla2x00_start_sp(sp);
	if (rval != QLA_SUCCESS) {
		ql_log(ql_log_warn, vha, 0x11004,
			"qla2x00_start_sp failed = %d\n", rval);
		return rval;
	}

	return 0;
}

/*
 * qla_nvmet_fcp_op -
 * Invoked by the nvme-t to complete the IO.
 * Prepare and send a response CTIO to the firmware.
 */
static int
qla_nvmet_fcp_op(struct nvmet_fc_target_port *tgtport,
			struct nvmefc_tgt_fcp_req *rsp)
{
	struct qla_nvmet_cmd *tgt_cmd =
		container_of(rsp, struct qla_nvmet_cmd, cmd.fcp_req);
	struct scsi_qla_host *vha = tgt_cmd->vha;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return 0;

	/* Prepare and send CTIO 82h */
	qla_nvmet_send_resp_ctio(vha->qpair, tgt_cmd, rsp);

	return 0;
}

/*
 * qla_nvmet_fcp_abort_done
 * free up the used resources
 */
static void qla_nvmet_fcp_abort_done(void *ptr, int res)
{
	srb_t *sp = ptr;

	qla2x00_rel_sp(sp);
}

/*
 * qla_nvmet_fcp_abort -
 * Invoked by the nvme-t to abort an IO
 * Send an abort to the firmware
 */
static void
qla_nvmet_fcp_abort(struct nvmet_fc_target_port *tgtport,
			struct nvmefc_tgt_fcp_req *req)
{
	struct qla_nvmet_cmd *tgt_cmd =
		container_of(req, struct qla_nvmet_cmd, cmd.fcp_req);
	struct scsi_qla_host *vha = tgt_cmd->vha;
	struct qla_hw_data *ha = vha->hw;
	srb_t *sp;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return;

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, NULL, GFP_KERNEL);
	if (!sp) {
		ql_log(ql_log_info, vha, 0x11005, "Failed to allocate SRB\n");
		return;
	}

	sp->type = SRB_NVMET_SEND_ABTS;
	sp->done = qla_nvmet_fcp_abort_done;
	sp->vha = vha;
	sp->fcport = tgt_cmd->fcport;

	ha->isp_ops->abort_command(sp);

}

/*
 * qla_nvmet_fcp_req_release -
 * Delete the cmd from the list and free the cmd
 */
static void
qla_nvmet_fcp_req_release(struct nvmet_fc_target_port *tgtport,
			struct nvmefc_tgt_fcp_req *rsp)
{
	struct qla_nvmet_cmd *tgt_cmd =
		container_of(rsp, struct qla_nvmet_cmd, cmd.fcp_req);
	scsi_qla_host_t *vha = tgt_cmd->vha;
	unsigned long flags;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return;

	spin_lock_irqsave(&vha->cmd_list_lock, flags);
	list_del(&tgt_cmd->cmd_list);
	spin_unlock_irqrestore(&vha->cmd_list_lock, flags);

	kfree(tgt_cmd);
}

static struct nvmet_fc_target_template qla_nvmet_fc_transport = {
	.targetport_delete	= qla_nvmet_targetport_delete,
	.xmt_ls_rsp		= qla_nvmet_ls_rsp,
	.fcp_op			= qla_nvmet_fcp_op,
	.fcp_abort		= qla_nvmet_fcp_abort,
	.fcp_req_release	= qla_nvmet_fcp_req_release,
	.max_hw_queues		= 8,
	.max_sgl_segments	= 128,
	.max_dif_sgl_segments	= 64,
	.dma_boundary		= 0xFFFFFFFF,
	.target_features	= NVMET_FCTGTFEAT_READDATA_RSP,
	.target_priv_sz	= sizeof(struct nvme_private),
};

/*
 * qla_nvmet_create_targetport -
 * Create a targetport. Registers the template with the nvme-t
 * layer
 */
int qla_nvmet_create_targetport(struct scsi_qla_host *vha)
{
	struct nvmet_fc_port_info pinfo;
	struct qla_nvmet_tgtport *tport;
	int error = 0;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return 0;

	ql_dbg(ql_dbg_nvme, vha, 0xe081,
		"Creating target port for :%p\n", vha);

	memset(&pinfo, 0, (sizeof(struct nvmet_fc_port_info)));
	pinfo.node_name = wwn_to_u64(vha->node_name);
	pinfo.port_name = wwn_to_u64(vha->port_name);
	pinfo.port_id	= vha->d_id.b24;

	error = nvmet_fc_register_targetport(&pinfo,
	    &qla_nvmet_fc_transport, &vha->hw->pdev->dev,
	    &vha->targetport);

	if (error) {
		ql_dbg(ql_dbg_nvme, vha, 0xe082,
			"Cannot register NVME transport:%d\n", error);
		return error;
	}
	tport = (struct qla_nvmet_tgtport *)vha->targetport->private;
	tport->vha = vha;
	ql_dbg(ql_dbg_nvme, vha, 0xe082,
		" Registered NVME transport:%p WWPN:%llx\n",
		tport, pinfo.port_name);
	return 0;
}

/*
 * qla_nvmet_delete -
 * Delete a targetport.
 */
int qla_nvmet_delete(struct scsi_qla_host *vha)
{
	struct qla_nvmet_tgtport *tport;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return 0;

	if (!vha->flags.nvmet_enabled)
		return 0;
	if (vha->targetport) {
		tport = (struct qla_nvmet_tgtport *)vha->targetport->private;

		ql_dbg(ql_dbg_nvme, vha, 0xe083,
			"Deleting target port :%p\n", tport);
		init_completion(&tport->tport_del);
		nvmet_fc_unregister_targetport(vha->targetport);
		wait_for_completion_timeout(&tport->tport_del, 5);

		nvmet_release_sessions(vha);
	}
	return 0;
}

/*
 * qla_nvmet_handle_ls -
 * Handle a link service request from the initiator.
 * Get the LS payload from the ATIO queue, invoke
 * nvmet_fc_rcv_ls_req to pass the LS req to nvmet.
 */
int qla_nvmet_handle_ls(struct scsi_qla_host *vha,
	struct pt_ls4_rx_unsol *pt_ls4, void *buf)
{
	struct qla_nvmet_cmd *tgt_cmd;
	uint32_t size;
	int ret;
	uint32_t look_up_sid;
	fc_port_t *sess = NULL;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return 0;

	look_up_sid = pt_ls4->s_id[2] << 16 |
	    pt_ls4->s_id[1] << 8 | pt_ls4->s_id[0];

	ql_dbg(ql_dbg_nvme, vha, 0x11005,
	    "%s - Look UP sid: %#x\n", __func__, look_up_sid);

	sess = qla_nvmet_find_sess_by_s_id(vha, look_up_sid);
	if (unlikely(!sess))
		WARN_ON(1);

	size = cpu_to_le16(pt_ls4->desc_len) + 8;

	tgt_cmd = kzalloc(sizeof(struct qla_nvmet_cmd), GFP_ATOMIC);
	if (tgt_cmd == NULL)
		return -ENOMEM;

	tgt_cmd->vha = vha;
	tgt_cmd->ox_id = pt_ls4->ox_id;
	tgt_cmd->buf = buf;
	/* Store the received nphdl, rx_exh_addr etc */
	memcpy(&tgt_cmd->atio.u.pt_ls4, pt_ls4, sizeof(struct pt_ls4_rx_unsol));
	tgt_cmd->fcport = sess;

	ql_dbg(ql_dbg_nvme + ql_dbg_buffer, vha, 0x11006,
	    "Dumping the PURLS-ATIO request\n");
	ql_dump_buffer(ql_dbg_nvme + ql_dbg_buffer, vha, 0x2075,
	    (uint8_t *)pt_ls4, sizeof(struct pt_ls4_rx_unsol));

	ql_dbg(ql_dbg_nvme, vha, 0x11007,
	    "Sending LS to nvmet buf: %p, len: %#x\n", buf, size);

	ret = nvmet_fc_rcv_ls_req(vha->targetport,
		&tgt_cmd->cmd.ls_req, buf, size);

	if (ret == 0) {
		ql_dbg(ql_dbg_nvme, vha, 0x11008,
		    "LS req handled successfully\n");
		return 0;
	}

	ql_log(ql_log_warn, vha, 0x11009, "LS req failed\n");

	return ret;
}

/*
 * qla_nvmet_process_cmd -
 * Handle NVME cmd request from the initiator.
 * Get the NVME payload from the ATIO queue, invoke
 * nvmet_fc_rcv_ls_req to pass the LS req to nvmet.
 * On a failure send an abts to the initiator?
 */
int qla_nvmet_process_cmd(struct scsi_qla_host *vha,
	struct qla_nvmet_cmd *tgt_cmd)
{
	int ret;
	struct atio7_nvme_cmnd *nvme_cmd;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return 0;

	nvme_cmd = (struct atio7_nvme_cmnd *)&tgt_cmd->nvme_cmd_iu;

	ret = nvmet_fc_rcv_fcp_req(vha->targetport, &tgt_cmd->cmd.fcp_req,
			nvme_cmd, tgt_cmd->cmd_len);
	if (ret != 0) {
		ql_log(ql_log_warn, vha, 0x1100a,
			"%s-%d - Failed (ret: %#x) to process NVME command\n",
				__func__, __LINE__, ret);
		/* Send ABTS to initator ? */
	}
	return 0;
}

/*
 * qla_nvmet_handle_abts
 * Handle an abort from the initiator
 * Invoke nvmet_fc_rcv_fcp_abort to pass the abts to the nvmet
 */
int qla_nvmet_handle_abts(struct scsi_qla_host *vha,
	struct abts_recv_from_24xx *abts)
{
	uint16_t ox_id = cpu_to_be16(abts->fcp_hdr_le.ox_id);
	unsigned long flags;
	struct qla_nvmet_cmd *cmd = NULL;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return 0;

	/* Retrieve the cmd from cmd list */
	spin_lock_irqsave(&vha->cmd_list_lock, flags);
	list_for_each_entry(cmd, &vha->qla_cmd_list, cmd_list) {
		if (cmd->ox_id == ox_id)
			break; /* Found the cmd */
	}
	spin_unlock_irqrestore(&vha->cmd_list_lock, flags);
	if (!cmd) {
		ql_log(ql_log_warn, vha, 0x1100b,
		    "%s-%d - Command not found\n", __func__, __LINE__);
		/* Send a RJT */
		qla_nvmet_send_abts_ctio(vha, abts, 0);
		return 0;
	}

	nvmet_fc_rcv_fcp_abort(vha->targetport, &cmd->cmd.fcp_req);
	/* Send an ACC */
	qla_nvmet_send_abts_ctio(vha, abts, 1);

	return 0;
}

/*
 * qla_nvmet_abts_done
 * Complete the cmd back to the nvme-t and
 * free up the used resources
 */
static void qla_nvmet_abts_done(void *ptr, int res)
{
	srb_t *sp = ptr;

	if (!IS_ENABLED(CONFIG_NVME_TARGET_FC))
		return;

	qla2x00_rel_sp(sp);
}
/*
 * qla_nvmet_fcp_done
 * Complete the cmd back to the nvme-t and
 * free up the used resources
 */
static void qla_nvmet_fcp_done(void *ptr, int res)
{
	srb_t *sp = ptr;
	struct nvmefc_tgt_fcp_req *rsp;

	rsp = sp->u.iocb_cmd.u.nvme.desc;

	if (res) {
		rsp->fcp_error = NVME_SC_SUCCESS;
		if (rsp->op == NVMET_FCOP_RSP)
			rsp->transferred_length = 0;
		else
			rsp->transferred_length = rsp->transfer_length;
	} else {
		rsp->fcp_error = NVME_SC_DATA_XFER_ERROR;
		rsp->transferred_length = 0;
	}
	rsp->done(rsp);
	qla2x00_rel_sp(sp);
}

/*
 * qla_nvmet_send_resp_ctio
 * Send the response CTIO to the firmware
 */
static void qla_nvmet_send_resp_ctio(struct qla_qpair *qpair,
	struct qla_nvmet_cmd *cmd, struct nvmefc_tgt_fcp_req *rsp_buf)
{
	struct atio_from_isp *atio = &cmd->atio;
	struct ctio_nvme_to_27xx *ctio;
	struct scsi_qla_host *vha = cmd->vha;
	struct qla_hw_data *ha = vha->hw;
	struct fcp_hdr *fchdr = &atio->u.nvme_isp27.fcp_hdr;
	srb_t *sp;
	unsigned long flags;
	uint16_t temp, c_flags = 0;
	struct req_que *req = vha->hw->req_q_map[0];
	uint32_t req_cnt = 1;
	uint32_t *cur_dsd;
	uint16_t avail_dsds;
	uint16_t tot_dsds, i, cnt;
	struct scatterlist *sgl, *sg;

	spin_lock_irqsave(&ha->hardware_lock, flags);

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, cmd->fcport, GFP_ATOMIC);
	if (!sp) {
		ql_log(ql_log_info, vha, 0x1100c, "Failed to allocate SRB\n");
		spin_unlock_irqrestore(&ha->hardware_lock, flags);
		return;
	}

	sp->type = SRB_NVMET_FCP;
	sp->name = "nvmet_fcp";
	sp->done = qla_nvmet_fcp_done;
	sp->u.iocb_cmd.u.nvme.desc = rsp_buf;
	sp->u.iocb_cmd.u.nvme.cmd = cmd;

	ctio = (struct ctio_nvme_to_27xx *)qla2x00_alloc_iocbs(vha, sp);
	if (!ctio) {
		ql_dbg(ql_dbg_nvme, vha, 0x3067,
		    "qla2x00t(%ld): %s failed: unable to allocate request packet",
		    vha->host_no, __func__);
		spin_unlock_irqrestore(&ha->hardware_lock, flags);
		return;
	}

	ctio->entry_type = CTIO_NVME;
	ctio->entry_count = 1;
	ctio->handle = sp->handle;
	ctio->nport_handle = cpu_to_le16(cmd->fcport->loop_id);
	ctio->timeout = cpu_to_le16(QLA_TGT_TIMEOUT);
	ctio->vp_index = vha->vp_idx;
	ctio->initiator_id[0] = fchdr->s_id[2];
	ctio->initiator_id[1] = fchdr->s_id[1];
	ctio->initiator_id[2] = fchdr->s_id[0];
	ctio->exchange_addr = atio->u.nvme_isp27.exchange_addr;
	temp = be16_to_cpu(fchdr->ox_id);
	ctio->ox_id = cpu_to_le16(temp);
	tot_dsds = ctio->dseg_count = cpu_to_le16(rsp_buf->sg_cnt);
	c_flags = atio->u.nvme_isp27.attr << 9;

	if ((ctio->dseg_count > 1) && (rsp_buf->op != NVMET_FCOP_RSP)) {
		/* Check for additional continuation IOCB space */
		req_cnt = qla24xx_calc_iocbs(vha, ctio->dseg_count);
		ctio->entry_count = req_cnt;

		if (req->cnt < (req_cnt + 2)) {
			cnt = (uint16_t)RD_REG_DWORD_RELAXED(req->req_q_out);

			if  (req->ring_index < cnt)
				req->cnt = cnt - req->ring_index;
			else
				req->cnt = req->length -
				    (req->ring_index - cnt);

			if (unlikely(req->cnt < (req_cnt + 2))) {
				ql_log(ql_log_warn, vha, 0xfff,
					"Running out of IOCB space for continuation IOCBs\n");
				goto err_exit;
			}
		}
	}

	switch (rsp_buf->op) {
	case NVMET_FCOP_READDATA:
	case NVMET_FCOP_READDATA_RSP:
		/* Populate the CTIO resp with the SGL present in the rsp */
		ql_dbg(ql_dbg_nvme, vha, 0x1100c,
		    "op: %#x, ox_id=%x c_flags=%x transfer_length: %#x req_cnt: %#x, tot_dsds: %#x\n",
		    rsp_buf->op, ctio->ox_id, c_flags,
		    rsp_buf->transfer_length, req_cnt, tot_dsds);

		avail_dsds = 1;
		cur_dsd = (uint32_t *)
				&ctio->u.nvme_status_mode0.dsd0[0];
		sgl = rsp_buf->sg;

		/* Load data segments */
		for_each_sg(sgl, sg, tot_dsds, i) {
			dma_addr_t      sle_dma;
			cont_a64_entry_t *cont_pkt;

			/* Allocate additional continuation packets? */
			if (avail_dsds == 0) {
				/*
				 * Five DSDs are available in the Cont
				 * Type 1 IOCB.
				 */

				/* Adjust ring index */
				req->ring_index++;
				if (req->ring_index == req->length) {
					req->ring_index = 0;
					req->ring_ptr = req->ring;
				} else {
					req->ring_ptr++;
				}
				cont_pkt = (cont_a64_entry_t *)
						req->ring_ptr;
				*((uint32_t *)(&cont_pkt->entry_type)) =
					cpu_to_le32(CONTINUE_A64_TYPE);

				cur_dsd = (uint32_t *)
						cont_pkt->dseg_0_address;
				avail_dsds = 5;
			}

			sle_dma = sg_dma_address(sg);
			*cur_dsd++ = cpu_to_le32(LSD(sle_dma));
			*cur_dsd++ = cpu_to_le32(MSD(sle_dma));
			*cur_dsd++ = cpu_to_le32(sg_dma_len(sg));
			avail_dsds--;
		}

		ctio->u.nvme_status_mode0.transfer_len =
			cpu_to_le32(rsp_buf->transfer_length);
		ctio->u.nvme_status_mode0.relative_offset =
			cpu_to_le32(rsp_buf->offset);
		ctio->flags = cpu_to_le16(c_flags | 0x2);

		if (rsp_buf->op == NVMET_FCOP_READDATA_RSP) {
			if (rsp_buf->rsplen == 12) {
				ctio->flags |=
					NVMET_CTIO_STS_MODE0 |
					NVMET_CTIO_SEND_STATUS;
			} else if (rsp_buf->rsplen == 32) {
				struct nvme_fc_ersp_iu *ersp =
				    rsp_buf->rspaddr;
				uint32_t iter = 4, *inbuf, *outbuf;

				ctio->flags |=
					NVMET_CTIO_STS_MODE1 |
					NVMET_CTIO_SEND_STATUS;
				inbuf = (uint32_t *)
					&((uint8_t *)rsp_buf->rspaddr)[16];
				outbuf = (uint32_t *)
				    ctio->u.nvme_status_mode1.nvme_comp_q_entry;
				for (; iter; iter--)
					*outbuf++ = cpu_to_be32(*inbuf++);

				ctio->u.nvme_status_mode1.rsp_seq_num =
					cpu_to_be32(ersp->rsn);
				ctio->u.nvme_status_mode1.transfer_len =
					cpu_to_be32(ersp->xfrd_len);
			} else {
				ql_log(ql_log_warn, vha, 0x1100d,
				    "unhandled resp len = %x\n", rsp_buf->rsplen);
			}
		}
		break;

	case NVMET_FCOP_WRITEDATA:
		/* Send transfer rdy */
		ql_dbg(ql_dbg_nvme, vha, 0x1100e,
		    "FCOP_WRITE: ox_id=%x c_flags=%x transfer_length: %#x req_cnt: %#x, tot_dsds: %#x\n",
		    ctio->ox_id, c_flags, rsp_buf->transfer_length,
		    req_cnt, tot_dsds);

		ctio->flags = cpu_to_le16(c_flags | 0x1);

		avail_dsds = 1;
		cur_dsd = (uint32_t *)&ctio->u.nvme_status_mode0.dsd0[0];
		sgl = rsp_buf->sg;

		/* Load data segments */
		for_each_sg(sgl, sg, tot_dsds, i) {
			dma_addr_t      sle_dma;
			cont_a64_entry_t *cont_pkt;

			/* Allocate additional continuation packets? */
			if (avail_dsds == 0) {
				/*
				 * Five DSDs are available in the Continuation
				 * Type 1 IOCB.
				 */

				/* Adjust ring index */
				req->ring_index++;
				if (req->ring_index == req->length) {
					req->ring_index = 0;
					req->ring_ptr = req->ring;
				} else {
					req->ring_ptr++;
				}
				cont_pkt = (cont_a64_entry_t *)req->ring_ptr;
				*((uint32_t *)(&cont_pkt->entry_type)) =
					cpu_to_le32(CONTINUE_A64_TYPE);

				cur_dsd = (uint32_t *)cont_pkt->dseg_0_address;
				avail_dsds = 5;
			}

			sle_dma = sg_dma_address(sg);
			*cur_dsd++ = cpu_to_le32(LSD(sle_dma));
			*cur_dsd++ = cpu_to_le32(MSD(sle_dma));
			*cur_dsd++ = cpu_to_le32(sg_dma_len(sg));
			avail_dsds--;
		}

		ctio->u.nvme_status_mode0.transfer_len =
			cpu_to_le32(rsp_buf->transfer_length);
		ctio->u.nvme_status_mode0.relative_offset =
			cpu_to_le32(rsp_buf->offset);

		break;
	case NVMET_FCOP_RSP:
		/* Send a response frame */
		ctio->flags = cpu_to_le16(c_flags);
		if (rsp_buf->rsplen == 12) {
			ctio->flags |=
			NVMET_CTIO_STS_MODE0 | NVMET_CTIO_SEND_STATUS;
		} else if (rsp_buf->rsplen == 32) {
			struct nvme_fc_ersp_iu *ersp = rsp_buf->rspaddr;
			uint32_t iter = 4, *inbuf, *outbuf;

			ctio->flags |=
				NVMET_CTIO_STS_MODE1 | NVMET_CTIO_SEND_STATUS;
			inbuf = (uint32_t *)
				&((uint8_t *)rsp_buf->rspaddr)[16];
			outbuf = (uint32_t *)
				ctio->u.nvme_status_mode1.nvme_comp_q_entry;
			for (; iter; iter--)
				*outbuf++ = cpu_to_be32(*inbuf++);
			ctio->u.nvme_status_mode1.rsp_seq_num =
						cpu_to_be32(ersp->rsn);
			ctio->u.nvme_status_mode1.transfer_len =
						cpu_to_be32(ersp->xfrd_len);

			ql_dbg(ql_dbg_nvme, vha, 0x1100f,
			    "op: %#x, rsplen: %#x\n", rsp_buf->op,
			    rsp_buf->rsplen);
		} else {
			ql_dbg(ql_dbg_nvme, vha, 0x11010,
			    "unhandled resp len = %x for op NVMET_FCOP_RSP\n",
			    rsp_buf->rsplen);
		}
		break;
	}

	/* Memory Barrier */
	wmb();

	qla2x00_start_iocbs(vha, vha->hw->req_q_map[0]);
err_exit:
	spin_unlock_irqrestore(&ha->hardware_lock, flags);
}

/*
 * qla_nvmet_send_abts_ctio
 * Send the abts CTIO to the firmware
 */
static void qla_nvmet_send_abts_ctio(struct scsi_qla_host *vha,
		struct abts_recv_from_24xx *rabts, bool flag)
{
	struct abts_resp_to_24xx *resp;
	srb_t *sp;
	uint32_t f_ctl;
	uint8_t *p;

	/* Alloc SRB structure */
	sp = qla2x00_get_sp(vha, NULL, GFP_ATOMIC);
	if (!sp) {
		ql_dbg(ql_dbg_nvme, vha, 0x11011, "Failed to allocate SRB\n");
		return;
	}

	sp->type = SRB_NVMET_ABTS;
	sp->name = "nvmet_abts";
	sp->done = qla_nvmet_abts_done;

	resp = (struct abts_resp_to_24xx *)qla2x00_alloc_iocbs(vha, sp);
	if (!resp) {
		ql_dbg(ql_dbg_nvme, vha, 0x3067,
		    "qla2x00t(%ld): %s failed: unable to allocate request packet",
		    vha->host_no, __func__);
		return;
	}

	resp->entry_type = ABTS_RESP_24XX;
	resp->entry_count = 1;
	resp->handle = sp->handle;

	resp->nport_handle = rabts->nport_handle;
	resp->vp_index = rabts->vp_index;
	resp->exchange_address = rabts->exchange_addr_to_abort;
	resp->fcp_hdr_le = rabts->fcp_hdr_le;
	f_ctl = cpu_to_le32(F_CTL_EXCH_CONTEXT_RESP |
	    F_CTL_LAST_SEQ | F_CTL_END_SEQ |
	    F_CTL_SEQ_INITIATIVE);
	p = (uint8_t *)&f_ctl;
	resp->fcp_hdr_le.f_ctl[0] = *p++;
	resp->fcp_hdr_le.f_ctl[1] = *p++;
	resp->fcp_hdr_le.f_ctl[2] = *p;

	resp->fcp_hdr_le.d_id[0] = rabts->fcp_hdr_le.s_id[0];
	resp->fcp_hdr_le.d_id[1] = rabts->fcp_hdr_le.s_id[1];
	resp->fcp_hdr_le.d_id[2] = rabts->fcp_hdr_le.s_id[2];
	resp->fcp_hdr_le.s_id[0] = rabts->fcp_hdr_le.d_id[0];
	resp->fcp_hdr_le.s_id[1] = rabts->fcp_hdr_le.d_id[1];
	resp->fcp_hdr_le.s_id[2] = rabts->fcp_hdr_le.d_id[2];

	if (flag) { /* BA_ACC */
		resp->fcp_hdr_le.r_ctl = R_CTL_BASIC_LINK_SERV | R_CTL_B_ACC;
		resp->payload.ba_acct.seq_id_valid = SEQ_ID_INVALID;
		resp->payload.ba_acct.low_seq_cnt = 0x0000;
		resp->payload.ba_acct.high_seq_cnt = 0xFFFF;
		resp->payload.ba_acct.ox_id = rabts->fcp_hdr_le.ox_id;
		resp->payload.ba_acct.rx_id = rabts->fcp_hdr_le.rx_id;
	} else {
		resp->fcp_hdr_le.r_ctl = R_CTL_BASIC_LINK_SERV | R_CTL_B_RJT;
		resp->payload.ba_rjt.reason_code =
			BA_RJT_REASON_CODE_UNABLE_TO_PERFORM;
	}
	/* Memory Barrier */
	wmb();

	qla2x00_start_iocbs(vha, vha->hw->req_q_map[0]);
}
