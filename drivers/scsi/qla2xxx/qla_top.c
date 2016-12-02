/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2016 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#include "qla_def.h"


int
qla2xxx_mqueuecommand(struct Scsi_Host *host, struct scsi_cmnd *cmd, struct qla_qpair *qpair)
{
	scsi_qla_host_t *vha = shost_priv(host);
	fc_port_t *fcport = (struct fc_port *) cmd->device->hostdata;
	struct fc_rport *rport = starget_to_rport(scsi_target(cmd->device));
	struct qla_hw_data *ha = vha->hw;
	struct scsi_qla_host *base_vha = pci_get_drvdata(ha->pdev);
	srb_t *sp;
	int rval;

	rval = fc_remote_port_chkready(rport);
	if (rval) {
		cmd->result = rval;
		ql_dbg(ql_dbg_io + ql_dbg_verbose, vha, 0x3076,
		    "fc_remote_port_chkready failed for cmd=%p, rval=0x%x.\n",
		    cmd, rval);
		goto qc24_fail_command;
	}

	if (!fcport) {
		cmd->result = DID_NO_CONNECT << 16;
		goto qc24_fail_command;
	}

	if (atomic_read(&fcport->state) != FCS_ONLINE) {
		if (atomic_read(&fcport->state) == FCS_DEVICE_DEAD ||
			atomic_read(&base_vha->loop_state) == LOOP_DEAD) {
			ql_dbg(ql_dbg_io, vha, 0x3077,
			    "Returning DNC, fcport_state=%d loop_state=%d.\n",
			    atomic_read(&fcport->state),
			    atomic_read(&base_vha->loop_state));
			cmd->result = DID_NO_CONNECT << 16;
			goto qc24_fail_command;
		}
		goto qc24_target_busy;
	}

	/*
	 * Return target busy if we've received a non-zero retry_delay_timer
	 * in a FCP_RSP.
	 */
	if (fcport->retry_delay_timestamp == 0) {
		/* retry delay not set */
	} else if (time_after(jiffies, fcport->retry_delay_timestamp))
		fcport->retry_delay_timestamp = 0;
	else
		goto qc24_target_busy;

	sp = qla2xxx_get_qpair_sp(qpair, fcport, GFP_ATOMIC);
	if (!sp)
		goto qc24_host_busy;

	sp->u.scmd.cmd = cmd;
	sp->type = SRB_SCSI_CMD;
	atomic_set(&sp->ref_count, 1);
	CMD_SP(cmd) = (void *)sp;
	sp->free = qla2xxx_qpair_sp_free_dma;
	sp->done = qla2xxx_qpair_sp_compl;
	sp->qpair = qpair;

	rval = ha->isp_ops->start_scsi_mq(sp);
	if (rval != QLA_SUCCESS) {
		ql_dbg(ql_dbg_io + ql_dbg_verbose, vha, 0x3078,
		    "Start scsi failed rval=%d for cmd=%p.\n", rval, cmd);
		if (rval == QLA_INTERFACE_ERROR)
			goto qc24_fail_command;
		goto qc24_host_busy_free_sp;
	}

	return 0;

qc24_host_busy_free_sp:
	qla2xxx_qpair_sp_free_dma(vha, sp);

qc24_host_busy:
	return SCSI_MLQUEUE_HOST_BUSY;

qc24_target_busy:
	return SCSI_MLQUEUE_TARGET_BUSY;

qc24_fail_command:
	cmd->scsi_done(cmd);

	return 0;
}
