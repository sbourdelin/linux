/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2016 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"

/**
 * qla2xxx_start_scsi_mq() - Send a SCSI command to the ISP
 * @sp: command to send to the ISP
 *
 * Returns non-zero if a failure occurred, else zero.
 */

static int
qla2xxx_start_scsi_mq(srb_t *sp)
{
	int		nseg;
	unsigned long   flags;
	uint32_t	*clr_ptr;
	uint32_t        index;
	uint32_t	handle;
	struct cmd_type_7 *cmd_pkt;
	uint16_t	cnt;
	uint16_t	req_cnt;
	uint16_t	tot_dsds;
	struct req_que *req = NULL;
	struct rsp_que *rsp = NULL;
	struct scsi_cmnd *cmd = GET_CMD_SP(sp);
	struct scsi_qla_host *vha = sp->fcport->vha;
	struct qla_hw_data *ha = vha->hw;
	struct qla_qpair *qpair = sp->qpair;

	/* Setup qpair pointers */
	rsp = qpair->rsp;
	req = qpair->req;

	/* So we know we haven't pci_map'ed anything yet */
	tot_dsds = 0;

	/* Send marker if required */
	if (vha->marker_needed != 0) {
		if (qla2x00_marker(vha, req, rsp, 0, 0, MK_SYNC_ALL) !=
		    QLA_SUCCESS)
			return QLA_FUNCTION_FAILED;
		vha->marker_needed = 0;
	}

	/* Acquire qpair specific lock */
	spin_lock_irqsave(&qpair->qp_lock, flags);

	/* Check for room in outstanding command list. */
	handle = req->current_outstanding_cmd;
	for (index = 1; index < req->num_outstanding_cmds; index++) {
		handle++;
		if (handle == req->num_outstanding_cmds)
			handle = 1;
		if (!req->outstanding_cmds[handle])
			break;
	}
	if (index == req->num_outstanding_cmds)
		goto queuing_error;

	/* Map the sg table so we have an accurate count of sg entries needed */
	if (scsi_sg_count(cmd)) {
		nseg = dma_map_sg(&ha->pdev->dev, scsi_sglist(cmd),
		    scsi_sg_count(cmd), cmd->sc_data_direction);
		if (unlikely(!nseg))
			goto queuing_error;
	} else
		nseg = 0;

	tot_dsds = nseg;
	req_cnt = qla24xx_calc_iocbs(vha, tot_dsds);
	if (req->cnt < (req_cnt + 2)) {
		cnt = IS_SHADOW_REG_CAPABLE(ha) ? *req->out_ptr :
		    RD_REG_DWORD_RELAXED(req->req_q_out);
		if (req->ring_index < cnt)
			req->cnt = cnt - req->ring_index;
		else
			req->cnt = req->length -
				(req->ring_index - cnt);
		if (req->cnt < (req_cnt + 2))
			goto queuing_error;
	}

	/* Build command packet. */
	req->current_outstanding_cmd = handle;
	req->outstanding_cmds[handle] = sp;
	sp->handle = handle;
	cmd->host_scribble = (unsigned char *)(unsigned long)handle;
	req->cnt -= req_cnt;

	cmd_pkt = (struct cmd_type_7 *)req->ring_ptr;
	cmd_pkt->handle = MAKE_HANDLE(req->id, handle);

	/* Zero out remaining portion of packet. */
	/*    tagged queuing modifier -- default is TSK_SIMPLE (0). */
	clr_ptr = (uint32_t *)cmd_pkt + 2;
	memset(clr_ptr, 0, REQUEST_ENTRY_SIZE - 8);
	cmd_pkt->dseg_count = cpu_to_le16(tot_dsds);

	/* Set NPORT-ID and LUN number*/
	cmd_pkt->nport_handle = cpu_to_le16(sp->fcport->loop_id);
	cmd_pkt->port_id[0] = sp->fcport->d_id.b.al_pa;
	cmd_pkt->port_id[1] = sp->fcport->d_id.b.area;
	cmd_pkt->port_id[2] = sp->fcport->d_id.b.domain;
	cmd_pkt->vp_index = sp->fcport->vha->vp_idx;

	int_to_scsilun(cmd->device->lun, &cmd_pkt->lun);
	host_to_fcp_swap((uint8_t *)&cmd_pkt->lun, sizeof(cmd_pkt->lun));

	cmd_pkt->task = TSK_SIMPLE;

	/* Load SCSI command packet. */
	memcpy(cmd_pkt->fcp_cdb, cmd->cmnd, cmd->cmd_len);
	host_to_fcp_swap(cmd_pkt->fcp_cdb, sizeof(cmd_pkt->fcp_cdb));

	cmd_pkt->byte_count = cpu_to_le32((uint32_t)scsi_bufflen(cmd));

	/* Build IOCB segments */
	qla24xx_build_scsi_iocbs(sp, cmd_pkt, tot_dsds, req);

	/* Set total data segment count. */
	cmd_pkt->entry_count = (uint8_t)req_cnt;
	wmb();
	/* Adjust ring index. */
	req->ring_index++;
	if (req->ring_index == req->length) {
		req->ring_index = 0;
		req->ring_ptr = req->ring;
	} else
		req->ring_ptr++;

	sp->flags |= SRB_DMA_VALID;

	/* Set chip new ring index. */
	WRT_REG_DWORD(req->req_q_in, req->ring_index);

	/* Manage unprocessed RIO/ZIO commands in response queue. */
	if (vha->flags.process_response_queue &&
		rsp->ring_ptr->signature != RESPONSE_PROCESSED)
		qla24xx_process_response_queue(vha, rsp);

	spin_unlock_irqrestore(&qpair->qp_lock, flags);
	return QLA_SUCCESS;

queuing_error:
	if (tot_dsds)
		scsi_dma_unmap(cmd);

	spin_unlock_irqrestore(&qpair->qp_lock, flags);

	return QLA_FUNCTION_FAILED;
}


/**
 * qla2xxx_dif_start_scsi_mq() - Send a SCSI command to the ISP
 * @sp: command to send to the ISP
 *
 * Returns non-zero if a failure occurred, else zero.
 */
int
qla2xxx_dif_start_scsi_mq(srb_t *sp)
{
	int			nseg;
	unsigned long		flags;
	uint32_t		*clr_ptr;
	uint32_t		index;
	uint32_t		handle;
	uint16_t		cnt;
	uint16_t		req_cnt = 0;
	uint16_t		tot_dsds;
	uint16_t		tot_prot_dsds;
	uint16_t		fw_prot_opts = 0;
	struct req_que		*req = NULL;
	struct rsp_que		*rsp = NULL;
	struct scsi_cmnd	*cmd = GET_CMD_SP(sp);
	struct scsi_qla_host	*vha = sp->fcport->vha;
	struct qla_hw_data	*ha = vha->hw;
	struct cmd_type_crc_2	*cmd_pkt;
	uint32_t		status = 0;
	struct qla_qpair	*qpair = sp->qpair;

#define QDSS_GOT_Q_SPACE	BIT_0

	/* Check for host side state */
	if (!qpair->online) {
		cmd->result = DID_NO_CONNECT << 16;
		return QLA_INTERFACE_ERROR;
	}

	if (!qpair->difdix_supported &&
		scsi_get_prot_op(cmd) != SCSI_PROT_NORMAL) {
		cmd->result = DID_NO_CONNECT << 16;
		return QLA_INTERFACE_ERROR;
	}

	/* Only process protection or >16 cdb in this routine */
	if (scsi_get_prot_op(cmd) == SCSI_PROT_NORMAL) {
		if (cmd->cmd_len <= 16)
			return qla2xxx_start_scsi_mq(sp);
	}

	/* Setup qpair pointers */
	rsp = qpair->rsp;
	req = qpair->req;

	/* So we know we haven't pci_map'ed anything yet */
	tot_dsds = 0;

	/* Send marker if required */
	if (vha->marker_needed != 0) {
		if (qla2x00_marker(vha, req, rsp, 0, 0, MK_SYNC_ALL) !=
		    QLA_SUCCESS)
			return QLA_FUNCTION_FAILED;
		vha->marker_needed = 0;
	}

	/* Acquire ring specific lock */
	spin_lock_irqsave(&qpair->qp_lock, flags);

	/* Check for room in outstanding command list. */
	handle = req->current_outstanding_cmd;
	for (index = 1; index < req->num_outstanding_cmds; index++) {
		handle++;
		if (handle == req->num_outstanding_cmds)
			handle = 1;
		if (!req->outstanding_cmds[handle])
			break;
	}

	if (index == req->num_outstanding_cmds)
		goto queuing_error;

	/* Compute number of required data segments */
	/* Map the sg table so we have an accurate count of sg entries needed */
	if (scsi_sg_count(cmd)) {
		nseg = dma_map_sg(&ha->pdev->dev, scsi_sglist(cmd),
		    scsi_sg_count(cmd), cmd->sc_data_direction);
		if (unlikely(!nseg))
			goto queuing_error;
		else
			sp->flags |= SRB_DMA_VALID;

		if ((scsi_get_prot_op(cmd) == SCSI_PROT_READ_INSERT) ||
		    (scsi_get_prot_op(cmd) == SCSI_PROT_WRITE_STRIP)) {
			struct qla2_sgx sgx;
			uint32_t	partial;

			memset(&sgx, 0, sizeof(struct qla2_sgx));
			sgx.tot_bytes = scsi_bufflen(cmd);
			sgx.cur_sg = scsi_sglist(cmd);
			sgx.sp = sp;

			nseg = 0;
			while (qla24xx_get_one_block_sg(
			    cmd->device->sector_size, &sgx, &partial))
				nseg++;
		}
	} else
		nseg = 0;

	/* number of required data segments */
	tot_dsds = nseg;

	/* Compute number of required protection segments */
	if (qla24xx_configure_prot_mode(sp, &fw_prot_opts)) {
		nseg = dma_map_sg(&ha->pdev->dev, scsi_prot_sglist(cmd),
		    scsi_prot_sg_count(cmd), cmd->sc_data_direction);
		if (unlikely(!nseg))
			goto queuing_error;
		else
			sp->flags |= SRB_CRC_PROT_DMA_VALID;

		if ((scsi_get_prot_op(cmd) == SCSI_PROT_READ_INSERT) ||
		    (scsi_get_prot_op(cmd) == SCSI_PROT_WRITE_STRIP)) {
			nseg = scsi_bufflen(cmd) / cmd->device->sector_size;
		}
	} else {
		nseg = 0;
	}

	req_cnt = 1;
	/* Total Data and protection sg segment(s) */
	tot_prot_dsds = nseg;
	tot_dsds += nseg;
	if (req->cnt < (req_cnt + 2)) {
		cnt = IS_SHADOW_REG_CAPABLE(ha) ? *req->out_ptr :
		    RD_REG_DWORD_RELAXED(req->req_q_out);
		if (req->ring_index < cnt)
			req->cnt = cnt - req->ring_index;
		else
			req->cnt = req->length -
				(req->ring_index - cnt);
		if (req->cnt < (req_cnt + 2))
			goto queuing_error;
	}

	status |= QDSS_GOT_Q_SPACE;

	/* Build header part of command packet (excluding the OPCODE). */
	req->current_outstanding_cmd = handle;
	req->outstanding_cmds[handle] = sp;
	sp->handle = handle;
	cmd->host_scribble = (unsigned char *)(unsigned long)handle;
	req->cnt -= req_cnt;

	/* Fill-in common area */
	cmd_pkt = (struct cmd_type_crc_2 *)req->ring_ptr;
	cmd_pkt->handle = MAKE_HANDLE(req->id, handle);

	clr_ptr = (uint32_t *)cmd_pkt + 2;
	memset(clr_ptr, 0, REQUEST_ENTRY_SIZE - 8);

	/* Set NPORT-ID and LUN number*/
	cmd_pkt->nport_handle = cpu_to_le16(sp->fcport->loop_id);
	cmd_pkt->port_id[0] = sp->fcport->d_id.b.al_pa;
	cmd_pkt->port_id[1] = sp->fcport->d_id.b.area;
	cmd_pkt->port_id[2] = sp->fcport->d_id.b.domain;

	int_to_scsilun(cmd->device->lun, &cmd_pkt->lun);
	host_to_fcp_swap((uint8_t *)&cmd_pkt->lun, sizeof(cmd_pkt->lun));

	/* Total Data and protection segment(s) */
	cmd_pkt->dseg_count = cpu_to_le16(tot_dsds);

	/* Build IOCB segments and adjust for data protection segments */
	if (qla24xx_build_scsi_crc_2_iocbs(sp, (struct cmd_type_crc_2 *)
	    req->ring_ptr, tot_dsds, tot_prot_dsds, fw_prot_opts) !=
		QLA_SUCCESS)
		goto queuing_error;

	cmd_pkt->entry_count = (uint8_t)req_cnt;
	cmd_pkt->timeout = cpu_to_le16(0);
	wmb();

	/* Adjust ring index. */
	req->ring_index++;
	if (req->ring_index == req->length) {
		req->ring_index = 0;
		req->ring_ptr = req->ring;
	} else
		req->ring_ptr++;

	/* Set chip new ring index. */
	WRT_REG_DWORD(req->req_q_in, req->ring_index);

	/* Manage unprocessed RIO/ZIO commands in response queue. */
	if (vha->flags.process_response_queue &&
	    rsp->ring_ptr->signature != RESPONSE_PROCESSED)
		qla24xx_process_response_queue(vha, rsp);

	spin_unlock_irqrestore(&qpair->qp_lock, flags);

	return QLA_SUCCESS;

queuing_error:
	if (status & QDSS_GOT_Q_SPACE) {
		req->outstanding_cmds[handle] = NULL;
		req->cnt += req_cnt;
	}
	/* Cleanup will be performed by the caller (queuecommand) */

	spin_unlock_irqrestore(&qpair->qp_lock, flags);
	return QLA_FUNCTION_FAILED;
}

irqreturn_t
qla2xxx_msix_rsp_q(int irq, void *dev_id)
{
	struct qla_hw_data *ha;
	struct qla_qpair *qpair;
	struct device_reg_24xx __iomem *reg;
	unsigned long flags;

	qpair = dev_id;
	if (!qpair) {
		ql_log(ql_log_info, NULL, 0x505b,
		    "%s: NULL response queue pointer.\n", __func__);
		return IRQ_NONE;
	}
	ha = qpair->hw;

	/* Clear the interrupt, if enabled, for this response queue */
	if (unlikely(!ha->flags.disable_msix_handshake)) {
		reg = &ha->iobase->isp24;
		spin_lock_irqsave(&ha->hardware_lock, flags);
		WRT_REG_DWORD(&reg->hccr, HCCRX_CLR_RISC_INT);
		spin_unlock_irqrestore(&ha->hardware_lock, flags);
	}

	queue_work(ha->wq, &qpair->q_work);

	return IRQ_HANDLED;
}
