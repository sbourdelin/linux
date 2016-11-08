/*
 * QLogic iSCSI Offload Driver
 * Copyright (c) 2016 Cavium Inc.
 *
 * This software is available under the terms of the GNU General Public License
 * (GPL) Version 2, available from the file COPYING in the main directory of
 * this source tree.
 */

#include <linux/blkdev.h>
#include <scsi/scsi_tcq.h>
#include <linux/delay.h>

#include "qedi.h"
#include "qedi_iscsi.h"
#include "qedi_gbl.h"

static int qedi_send_iscsi_tmf(struct qedi_conn *qedi_conn,
			       struct iscsi_task *mtask);

void qedi_iscsi_unmap_sg_list(struct qedi_cmd *cmd)
{
	struct scsi_cmnd *sc = cmd->scsi_cmd;

	if (cmd->io_tbl.sge_valid && sc) {
		cmd->io_tbl.sge_valid = 0;
		scsi_dma_unmap(sc);
	}
}

static void qedi_process_logout_resp(struct qedi_ctx *qedi,
				     union iscsi_cqe *cqe,
				     struct iscsi_task *task,
				     struct qedi_conn *qedi_conn)
{
	struct iscsi_conn *conn = qedi_conn->cls_conn->dd_data;
	struct iscsi_logout_rsp *resp_hdr;
	struct iscsi_session *session = conn->session;
	struct iscsi_logout_response_hdr *cqe_logout_response;
	struct qedi_cmd *cmd;

	cmd = (struct qedi_cmd *)task->dd_data;
	cqe_logout_response = &cqe->cqe_common.iscsi_hdr.logout_response;
	spin_lock(&session->back_lock);
	resp_hdr = (struct iscsi_logout_rsp *)&qedi_conn->gen_pdu.resp_hdr;
	memset(resp_hdr, 0, sizeof(struct iscsi_hdr));
	resp_hdr->opcode = cqe_logout_response->opcode;
	resp_hdr->flags = cqe_logout_response->flags;
	resp_hdr->hlength = 0;

	resp_hdr->itt = build_itt(cqe->cqe_solicited.itid, conn->session->age);
	resp_hdr->statsn = cpu_to_be32(cqe_logout_response->stat_sn);
	resp_hdr->exp_cmdsn = cpu_to_be32(cqe_logout_response->exp_cmd_sn);
	resp_hdr->max_cmdsn = cpu_to_be32(cqe_logout_response->max_cmd_sn);

	resp_hdr->t2wait = cpu_to_be32(cqe_logout_response->time2wait);
	resp_hdr->t2retain = cpu_to_be32(cqe_logout_response->time2retain);

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_TID,
		  "Freeing tid=0x%x for cid=0x%x\n",
		  cmd->task_id, qedi_conn->iscsi_conn_id);

	if (likely(cmd->io_cmd_in_list)) {
		cmd->io_cmd_in_list = false;
		list_del_init(&cmd->io_cmd);
		qedi_conn->active_cmd_count--;
	} else {
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
			  "Active cmd list node already deleted, tid=0x%x, cid=0x%x, io_cmd_node=%p\n",
			  cmd->task_id, qedi_conn->iscsi_conn_id,
			  &cmd->io_cmd);
	}

	cmd->state = RESPONSE_RECEIVED;
	qedi_clear_task_idx(qedi, cmd->task_id);
	__iscsi_complete_pdu(conn, (struct iscsi_hdr *)resp_hdr, NULL, 0);

	spin_unlock(&session->back_lock);
}

static void qedi_process_text_resp(struct qedi_ctx *qedi,
				   union iscsi_cqe *cqe,
				   struct iscsi_task *task,
				   struct qedi_conn *qedi_conn)
{
	struct iscsi_conn *conn = qedi_conn->cls_conn->dd_data;
	struct iscsi_session *session = conn->session;
	struct iscsi_task_context *task_ctx;
	struct iscsi_text_rsp *resp_hdr_ptr;
	struct iscsi_text_response_hdr *cqe_text_response;
	struct qedi_cmd *cmd;
	int pld_len;
	u32 *tmp;

	cmd = (struct qedi_cmd *)task->dd_data;
	task_ctx = qedi_get_task_mem(&qedi->tasks, cmd->task_id);

	cqe_text_response = &cqe->cqe_common.iscsi_hdr.text_response;
	spin_lock(&session->back_lock);
	resp_hdr_ptr =  (struct iscsi_text_rsp *)&qedi_conn->gen_pdu.resp_hdr;
	memset(resp_hdr_ptr, 0, sizeof(struct iscsi_hdr));
	resp_hdr_ptr->opcode = cqe_text_response->opcode;
	resp_hdr_ptr->flags = cqe_text_response->flags;
	resp_hdr_ptr->hlength = 0;

	hton24(resp_hdr_ptr->dlength,
	       (cqe_text_response->hdr_second_dword &
		ISCSI_TEXT_RESPONSE_HDR_DATA_SEG_LEN_MASK));
	tmp = (u32 *)resp_hdr_ptr->dlength;

	resp_hdr_ptr->itt = build_itt(cqe->cqe_solicited.itid,
				      conn->session->age);
	resp_hdr_ptr->ttt = cqe_text_response->ttt;
	resp_hdr_ptr->statsn = cpu_to_be32(cqe_text_response->stat_sn);
	resp_hdr_ptr->exp_cmdsn = cpu_to_be32(cqe_text_response->exp_cmd_sn);
	resp_hdr_ptr->max_cmdsn = cpu_to_be32(cqe_text_response->max_cmd_sn);

	pld_len = cqe_text_response->hdr_second_dword &
		  ISCSI_TEXT_RESPONSE_HDR_DATA_SEG_LEN_MASK;
	qedi_conn->gen_pdu.resp_wr_ptr = qedi_conn->gen_pdu.resp_buf + pld_len;

	memset(task_ctx, '\0', sizeof(*task_ctx));

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_TID,
		  "Freeing tid=0x%x for cid=0x%x\n",
		  cmd->task_id, qedi_conn->iscsi_conn_id);

	if (likely(cmd->io_cmd_in_list)) {
		cmd->io_cmd_in_list = false;
		list_del_init(&cmd->io_cmd);
		qedi_conn->active_cmd_count--;
	} else {
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_INFO,
			  "Active cmd list node already deleted, tid=0x%x, cid=0x%x, io_cmd_node=%p\n",
			  cmd->task_id, qedi_conn->iscsi_conn_id,
			  &cmd->io_cmd);
	}

	cmd->state = RESPONSE_RECEIVED;
	qedi_clear_task_idx(qedi, cmd->task_id);

	__iscsi_complete_pdu(conn, (struct iscsi_hdr *)resp_hdr_ptr,
			     qedi_conn->gen_pdu.resp_buf,
			     (qedi_conn->gen_pdu.resp_wr_ptr -
			      qedi_conn->gen_pdu.resp_buf));
	spin_unlock(&session->back_lock);
}

static void qedi_process_login_resp(struct qedi_ctx *qedi,
				    union iscsi_cqe *cqe,
				    struct iscsi_task *task,
				    struct qedi_conn *qedi_conn)
{
	struct iscsi_conn *conn = qedi_conn->cls_conn->dd_data;
	struct iscsi_session *session = conn->session;
	struct iscsi_task_context *task_ctx;
	struct iscsi_login_rsp *resp_hdr_ptr;
	struct iscsi_login_response_hdr *cqe_login_response;
	struct qedi_cmd *cmd;
	int pld_len;
	u32 *tmp;

	cmd = (struct qedi_cmd *)task->dd_data;

	cqe_login_response = &cqe->cqe_common.iscsi_hdr.login_response;
	task_ctx = qedi_get_task_mem(&qedi->tasks, cmd->task_id);

	spin_lock(&session->back_lock);
	resp_hdr_ptr =  (struct iscsi_login_rsp *)&qedi_conn->gen_pdu.resp_hdr;
	memset(resp_hdr_ptr, 0, sizeof(struct iscsi_login_rsp));
	resp_hdr_ptr->opcode = cqe_login_response->opcode;
	resp_hdr_ptr->flags = cqe_login_response->flags_attr;
	resp_hdr_ptr->hlength = 0;

	hton24(resp_hdr_ptr->dlength,
	       (cqe_login_response->hdr_second_dword &
		ISCSI_LOGIN_RESPONSE_HDR_DATA_SEG_LEN_MASK));
	tmp = (u32 *)resp_hdr_ptr->dlength;
	resp_hdr_ptr->itt = build_itt(cqe->cqe_solicited.itid,
				      conn->session->age);
	resp_hdr_ptr->tsih = cqe_login_response->tsih;
	resp_hdr_ptr->statsn = cpu_to_be32(cqe_login_response->stat_sn);
	resp_hdr_ptr->exp_cmdsn = cpu_to_be32(cqe_login_response->exp_cmd_sn);
	resp_hdr_ptr->max_cmdsn = cpu_to_be32(cqe_login_response->max_cmd_sn);
	resp_hdr_ptr->status_class = cqe_login_response->status_class;
	resp_hdr_ptr->status_detail = cqe_login_response->status_detail;
	pld_len = cqe_login_response->hdr_second_dword &
		  ISCSI_LOGIN_RESPONSE_HDR_DATA_SEG_LEN_MASK;
	qedi_conn->gen_pdu.resp_wr_ptr = qedi_conn->gen_pdu.resp_buf + pld_len;

	if (likely(cmd->io_cmd_in_list)) {
		cmd->io_cmd_in_list = false;
		list_del_init(&cmd->io_cmd);
		qedi_conn->active_cmd_count--;
	}

	memset(task_ctx, '\0', sizeof(*task_ctx));

	__iscsi_complete_pdu(conn, (struct iscsi_hdr *)resp_hdr_ptr,
			     qedi_conn->gen_pdu.resp_buf,
			     (qedi_conn->gen_pdu.resp_wr_ptr -
			     qedi_conn->gen_pdu.resp_buf));

	spin_unlock(&session->back_lock);
	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_TID,
		  "Freeing tid=0x%x for cid=0x%x\n",
		  cmd->task_id, qedi_conn->iscsi_conn_id);
	cmd->state = RESPONSE_RECEIVED;
	qedi_clear_task_idx(qedi, cmd->task_id);
}

static void qedi_get_rq_bdq_buf(struct qedi_ctx *qedi,
				struct iscsi_cqe_unsolicited *cqe,
				char *ptr, int len)
{
	u16 idx = 0;

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
		  "pld_len [%d], bdq_prod_idx [%d], idx [%d]\n",
		  len, qedi->bdq_prod_idx,
		  (qedi->bdq_prod_idx % qedi->rq_num_entries));

	/* Obtain buffer address from rqe_opaque */
	idx = cqe->rqe_opaque.lo;
	if ((idx < 0) || (idx > (QEDI_BDQ_NUM - 1))) {
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
			  "wrong idx %d returned by FW, dropping the unsolicited pkt\n",
			  idx);
		return;
	}

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
		  "rqe_opaque.lo [0x%p], rqe_opaque.hi [0x%p], idx [%d]\n",
		  cqe->rqe_opaque.lo, cqe->rqe_opaque.hi, idx);

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
		  "unsol_cqe_type = %d\n", cqe->unsol_cqe_type);
	switch (cqe->unsol_cqe_type) {
	case ISCSI_CQE_UNSOLICITED_SINGLE:
	case ISCSI_CQE_UNSOLICITED_FIRST:
		if (len)
			memcpy(ptr, (void *)qedi->bdq[idx].buf_addr, len);
		break;
	case ISCSI_CQE_UNSOLICITED_MIDDLE:
	case ISCSI_CQE_UNSOLICITED_LAST:
		break;
	default:
		break;
	}
}

static void qedi_put_rq_bdq_buf(struct qedi_ctx *qedi,
				struct iscsi_cqe_unsolicited *cqe,
				int count)
{
	u16 tmp;
	u16 idx = 0;
	struct scsi_bd *pbl;

	/* Obtain buffer address from rqe_opaque */
	idx = cqe->rqe_opaque.lo;
	if ((idx < 0) || (idx > (QEDI_BDQ_NUM - 1))) {
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
			  "wrong idx %d returned by FW, dropping the unsolicited pkt\n",
			  idx);
		return;
	}

	pbl = (struct scsi_bd *)qedi->bdq_pbl;
	pbl += (qedi->bdq_prod_idx % qedi->rq_num_entries);
	pbl->address.hi = cpu_to_le32(QEDI_U64_HI(qedi->bdq[idx].buf_dma));
	pbl->address.lo = cpu_to_le32(QEDI_U64_LO(qedi->bdq[idx].buf_dma));
	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
		  "pbl [0x%p] pbl->address hi [0x%llx] lo [0x%llx] idx [%d]\n",
		  pbl, pbl->address.hi, pbl->address.lo, idx);
	pbl->opaque.hi = 0;
	pbl->opaque.lo = cpu_to_le32(QEDI_U64_LO(idx));

	/* Increment producer to let f/w know we've handled the frame */
	qedi->bdq_prod_idx += count;

	writew(qedi->bdq_prod_idx, qedi->bdq_primary_prod);
	tmp = readw(qedi->bdq_primary_prod);

	writew(qedi->bdq_prod_idx, qedi->bdq_secondary_prod);
	tmp = readw(qedi->bdq_secondary_prod);
}

static void qedi_unsol_pdu_adjust_bdq(struct qedi_ctx *qedi,
				      struct iscsi_cqe_unsolicited *cqe,
				      u32 pdu_len, u32 num_bdqs,
				      char *bdq_data)
{
	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
		  "num_bdqs [%d]\n", num_bdqs);

	qedi_get_rq_bdq_buf(qedi, cqe, bdq_data, pdu_len);
	qedi_put_rq_bdq_buf(qedi, cqe, (num_bdqs + 1));
}

static int qedi_process_nopin_mesg(struct qedi_ctx *qedi,
				   union iscsi_cqe *cqe,
				   struct iscsi_task *task,
				   struct qedi_conn *qedi_conn, u16 que_idx)
{
	struct iscsi_conn *conn = qedi_conn->cls_conn->dd_data;
	struct iscsi_session *session = conn->session;
	struct iscsi_nop_in_hdr *cqe_nop_in;
	struct iscsi_nopin *hdr;
	struct qedi_cmd *cmd;
	int tgt_async_nop = 0;
	u32 lun[2];
	u32 pdu_len, num_bdqs;
	char bdq_data[QEDI_BDQ_BUF_SIZE];
	unsigned long flags;

	spin_lock_bh(&session->back_lock);
	cqe_nop_in = &cqe->cqe_common.iscsi_hdr.nop_in;

	pdu_len = cqe_nop_in->hdr_second_dword &
		  ISCSI_NOP_IN_HDR_DATA_SEG_LEN_MASK;
	num_bdqs = pdu_len / QEDI_BDQ_BUF_SIZE;

	hdr = (struct iscsi_nopin *)&qedi_conn->gen_pdu.resp_hdr;
	memset(hdr, 0, sizeof(struct iscsi_hdr));
	hdr->opcode = cqe_nop_in->opcode;
	hdr->max_cmdsn = cpu_to_be32(cqe_nop_in->max_cmd_sn);
	hdr->exp_cmdsn = cpu_to_be32(cqe_nop_in->exp_cmd_sn);
	hdr->statsn = cpu_to_be32(cqe_nop_in->stat_sn);
	hdr->ttt = cpu_to_be32(cqe_nop_in->ttt);

	if (cqe->cqe_common.cqe_type == ISCSI_CQE_TYPE_UNSOLICITED) {
		spin_lock_irqsave(&qedi->hba_lock, flags);
		qedi_unsol_pdu_adjust_bdq(qedi, &cqe->cqe_unsolicited,
					  pdu_len, num_bdqs, bdq_data);
		hdr->itt = RESERVED_ITT;
		tgt_async_nop = 1;
		spin_unlock_irqrestore(&qedi->hba_lock, flags);
		goto done;
	}

	/* Response to one of our nop-outs */
	if (task) {
		cmd = task->dd_data;
		hdr->flags = ISCSI_FLAG_CMD_FINAL;
		hdr->itt = build_itt(cqe->cqe_solicited.itid,
				     conn->session->age);
		lun[0] = 0xffffffff;
		lun[1] = 0xffffffff;
		memcpy(&hdr->lun, lun, sizeof(struct scsi_lun));
		QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_TID,
			  "Freeing tid=0x%x for cid=0x%x\n",
			  cmd->task_id, qedi_conn->iscsi_conn_id);
		cmd->state = RESPONSE_RECEIVED;
		spin_lock(&qedi_conn->list_lock);
		if (likely(cmd->io_cmd_in_list)) {
			cmd->io_cmd_in_list = false;
			list_del_init(&cmd->io_cmd);
			qedi_conn->active_cmd_count--;
		}

		spin_unlock(&qedi_conn->list_lock);
		qedi_clear_task_idx(qedi, cmd->task_id);
	}

done:
	__iscsi_complete_pdu(conn, (struct iscsi_hdr *)hdr, bdq_data, pdu_len);

	spin_unlock_bh(&session->back_lock);
	return tgt_async_nop;
}

static void qedi_process_async_mesg(struct qedi_ctx *qedi,
				    union iscsi_cqe *cqe,
				    struct iscsi_task *task,
				    struct qedi_conn *qedi_conn,
				    u16 que_idx)
{
	struct iscsi_conn *conn = qedi_conn->cls_conn->dd_data;
	struct iscsi_session *session = conn->session;
	struct iscsi_async_msg_hdr *cqe_async_msg;
	struct iscsi_async *resp_hdr;
	u32 lun[2];
	u32 pdu_len, num_bdqs;
	char bdq_data[QEDI_BDQ_BUF_SIZE];
	unsigned long flags;

	spin_lock_bh(&session->back_lock);

	cqe_async_msg = &cqe->cqe_common.iscsi_hdr.async_msg;
	pdu_len = cqe_async_msg->hdr_second_dword &
		ISCSI_ASYNC_MSG_HDR_DATA_SEG_LEN_MASK;
	num_bdqs = pdu_len / QEDI_BDQ_BUF_SIZE;

	if (cqe->cqe_common.cqe_type == ISCSI_CQE_TYPE_UNSOLICITED) {
		spin_lock_irqsave(&qedi->hba_lock, flags);
		qedi_unsol_pdu_adjust_bdq(qedi, &cqe->cqe_unsolicited,
					  pdu_len, num_bdqs, bdq_data);
		spin_unlock_irqrestore(&qedi->hba_lock, flags);
	}

	resp_hdr = (struct iscsi_async *)&qedi_conn->gen_pdu.resp_hdr;
	memset(resp_hdr, 0, sizeof(struct iscsi_hdr));
	resp_hdr->opcode = cqe_async_msg->opcode;
	resp_hdr->flags = 0x80;

	lun[0] = cpu_to_be32(cqe_async_msg->lun.lo);
	lun[1] = cpu_to_be32(cqe_async_msg->lun.hi);
	memcpy(&resp_hdr->lun, lun, sizeof(struct scsi_lun));
	resp_hdr->exp_cmdsn = cpu_to_be32(cqe_async_msg->exp_cmd_sn);
	resp_hdr->max_cmdsn = cpu_to_be32(cqe_async_msg->max_cmd_sn);
	resp_hdr->statsn = cpu_to_be32(cqe_async_msg->stat_sn);

	resp_hdr->async_event = cqe_async_msg->async_event;
	resp_hdr->async_vcode = cqe_async_msg->async_vcode;

	resp_hdr->param1 = cpu_to_be16(cqe_async_msg->param1_rsrv);
	resp_hdr->param2 = cpu_to_be16(cqe_async_msg->param2_rsrv);
	resp_hdr->param3 = cpu_to_be16(cqe_async_msg->param3_rsrv);

	__iscsi_complete_pdu(conn, (struct iscsi_hdr *)resp_hdr, bdq_data,
			     pdu_len);

	spin_unlock_bh(&session->back_lock);
}

static void qedi_process_reject_mesg(struct qedi_ctx *qedi,
				     union iscsi_cqe *cqe,
				     struct iscsi_task *task,
				     struct qedi_conn *qedi_conn,
				     uint16_t que_idx)
{
	struct iscsi_conn *conn = qedi_conn->cls_conn->dd_data;
	struct iscsi_session *session = conn->session;
	struct iscsi_reject_hdr *cqe_reject;
	struct iscsi_reject *hdr;
	u32 pld_len, num_bdqs;
	unsigned long flags;

	spin_lock_bh(&session->back_lock);
	cqe_reject = &cqe->cqe_common.iscsi_hdr.reject;
	pld_len = cqe_reject->hdr_second_dword &
		  ISCSI_REJECT_HDR_DATA_SEG_LEN_MASK;
	num_bdqs = pld_len / QEDI_BDQ_BUF_SIZE;

	if (cqe->cqe_common.cqe_type == ISCSI_CQE_TYPE_UNSOLICITED) {
		spin_lock_irqsave(&qedi->hba_lock, flags);
		qedi_unsol_pdu_adjust_bdq(qedi, &cqe->cqe_unsolicited,
					  pld_len, num_bdqs, conn->data);
		spin_unlock_irqrestore(&qedi->hba_lock, flags);
	}
	hdr = (struct iscsi_reject *)&qedi_conn->gen_pdu.resp_hdr;
	memset(hdr, 0, sizeof(struct iscsi_hdr));
	hdr->opcode = cqe_reject->opcode;
	hdr->reason = cqe_reject->hdr_reason;
	hdr->flags = cqe_reject->hdr_flags;
	hton24(hdr->dlength, (cqe_reject->hdr_second_dword &
			      ISCSI_REJECT_HDR_DATA_SEG_LEN_MASK));
	hdr->max_cmdsn = cpu_to_be32(cqe_reject->max_cmd_sn);
	hdr->exp_cmdsn = cpu_to_be32(cqe_reject->exp_cmd_sn);
	hdr->statsn = cpu_to_be32(cqe_reject->stat_sn);
	hdr->ffffffff = cpu_to_be32(0xffffffff);

	__iscsi_complete_pdu(conn, (struct iscsi_hdr *)hdr,
			     conn->data, pld_len);
	spin_unlock_bh(&session->back_lock);
}

static void qedi_mtask_completion(struct qedi_ctx *qedi,
				  union iscsi_cqe *cqe,
				  struct iscsi_task *task,
				  struct qedi_conn *conn, uint16_t que_idx)
{
	struct iscsi_conn *iscsi_conn;
	u32 hdr_opcode;

	hdr_opcode = cqe->cqe_common.iscsi_hdr.common.hdr_first_byte;
	iscsi_conn = conn->cls_conn->dd_data;

	switch (hdr_opcode) {
	case ISCSI_OPCODE_LOGIN_RESPONSE:
		qedi_process_login_resp(qedi, cqe, task, conn);
		break;
	case ISCSI_OPCODE_TEXT_RESPONSE:
		qedi_process_text_resp(qedi, cqe, task, conn);
		break;
	case ISCSI_OPCODE_LOGOUT_RESPONSE:
		qedi_process_logout_resp(qedi, cqe, task, conn);
		break;
	case ISCSI_OPCODE_NOP_IN:
		qedi_process_nopin_mesg(qedi, cqe, task, conn, que_idx);
		break;
	default:
		QEDI_ERR(&qedi->dbg_ctx, "unknown opcode\n");
	}
}

static void qedi_process_nopin_local_cmpl(struct qedi_ctx *qedi,
					  struct iscsi_cqe_solicited *cqe,
					  struct iscsi_task *task,
					  struct qedi_conn *qedi_conn)
{
	struct iscsi_conn *conn = qedi_conn->cls_conn->dd_data;
	struct iscsi_session *session = conn->session;
	struct qedi_cmd *cmd = task->dd_data;

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_UNSOL,
		  "itid=0x%x, cmd task id=0x%x\n",
		  cqe->itid, cmd->task_id);

	cmd->state = RESPONSE_RECEIVED;
	qedi_clear_task_idx(qedi, cmd->task_id);

	spin_lock_bh(&session->back_lock);
	__iscsi_put_task(task);
	spin_unlock_bh(&session->back_lock);
}

void qedi_fp_process_cqes(struct qedi_work *work)
{
	struct qedi_ctx *qedi = work->qedi;
	union iscsi_cqe *cqe = &work->cqe;
	struct iscsi_task *task = NULL;
	struct iscsi_nopout *nopout_hdr;
	struct qedi_conn *q_conn;
	struct iscsi_conn *conn;
	struct qedi_cmd *qedi_cmd;
	u32 comp_type;
	u32 iscsi_cid;
	u32 hdr_opcode;
	u16 que_idx = work->que_idx;
	u8 cqe_err_bits = 0;

	comp_type = cqe->cqe_common.cqe_type;
	hdr_opcode = cqe->cqe_common.iscsi_hdr.common.hdr_first_byte;
	cqe_err_bits =
		cqe->cqe_common.error_bitmap.error_bits.cqe_error_status_bits;

	QEDI_INFO(&qedi->dbg_ctx, QEDI_LOG_CONN,
		  "fw_cid=0x%x, cqe type=0x%x, opcode=0x%x\n",
		  cqe->cqe_common.conn_id, comp_type, hdr_opcode);

	if (comp_type >= MAX_ISCSI_CQES_TYPE) {
		QEDI_WARN(&qedi->dbg_ctx, "Invalid CqE type\n");
		return;
	}

	iscsi_cid  = cqe->cqe_common.conn_id;
	q_conn = qedi->cid_que.conn_cid_tbl[iscsi_cid];
	if (!q_conn) {
		QEDI_WARN(&qedi->dbg_ctx,
			  "Session no longer exists for cid=0x%x!!\n",
			  iscsi_cid);
		return;
	}

	conn = q_conn->cls_conn->dd_data;

	if (unlikely(cqe_err_bits &&
		     GET_FIELD(cqe_err_bits,
			       CQE_ERROR_BITMAP_DATA_DIGEST_ERR))) {
		iscsi_conn_failure(conn, ISCSI_ERR_DATA_DGST);
		return;
	}

	switch (comp_type) {
	case ISCSI_CQE_TYPE_SOLICITED:
	case ISCSI_CQE_TYPE_SOLICITED_WITH_SENSE:
		qedi_cmd = container_of(work, struct qedi_cmd, cqe_work);
		task = qedi_cmd->task;
		if (!task) {
			QEDI_WARN(&qedi->dbg_ctx, "task is NULL\n");
			return;
		}

		/* Process NOPIN local completion */
		nopout_hdr = (struct iscsi_nopout *)task->hdr;
		if ((nopout_hdr->itt == RESERVED_ITT) &&
		    (cqe->cqe_solicited.itid != (u16)RESERVED_ITT)) {
			qedi_process_nopin_local_cmpl(qedi, &cqe->cqe_solicited,
						      task, q_conn);
		} else {
			cqe->cqe_solicited.itid =
					       qedi_get_itt(cqe->cqe_solicited);
			/* Process other solicited responses */
			qedi_mtask_completion(qedi, cqe, task, q_conn, que_idx);
		}
		break;
	case ISCSI_CQE_TYPE_UNSOLICITED:
		switch (hdr_opcode) {
		case ISCSI_OPCODE_NOP_IN:
			qedi_process_nopin_mesg(qedi, cqe, task, q_conn,
						que_idx);
			break;
		case ISCSI_OPCODE_ASYNC_MSG:
			qedi_process_async_mesg(qedi, cqe, task, q_conn,
						que_idx);
			break;
		case ISCSI_OPCODE_REJECT:
			qedi_process_reject_mesg(qedi, cqe, task, q_conn,
						 que_idx);
			break;
		}
		goto exit_fp_process;
	default:
		QEDI_ERR(&qedi->dbg_ctx, "Error cqe.\n");
		break;
	}

exit_fp_process:
	return;
}

static void qedi_add_to_sq(struct qedi_conn *qedi_conn, struct iscsi_task *task,
			   u16 tid, uint16_t ptu_invalidate, int is_cleanup)
{
	struct iscsi_wqe *wqe;
	struct iscsi_wqe_field *cont_field;
	struct qedi_endpoint *ep;
	struct scsi_cmnd *sc = task->sc;
	struct iscsi_login_req *login_hdr;
	struct qedi_cmd *cmd = task->dd_data;

	login_hdr = (struct iscsi_login_req *)task->hdr;
	ep = qedi_conn->ep;
	wqe = &ep->sq[ep->sq_prod_idx];

	memset(wqe, 0, sizeof(*wqe));

	ep->sq_prod_idx++;
	ep->fw_sq_prod_idx++;
	if (ep->sq_prod_idx == QEDI_SQ_SIZE)
		ep->sq_prod_idx = 0;

	if (is_cleanup) {
		SET_FIELD(wqe->flags, ISCSI_WQE_WQE_TYPE,
			  ISCSI_WQE_TYPE_TASK_CLEANUP);
		wqe->task_id = tid;
		return;
	}

	if (ptu_invalidate) {
		SET_FIELD(wqe->flags, ISCSI_WQE_PTU_INVALIDATE,
			  ISCSI_WQE_SET_PTU_INVALIDATE);
	}

	cont_field = &wqe->cont_prevtid_union.cont_field;

	switch (task->hdr->opcode & ISCSI_OPCODE_MASK) {
	case ISCSI_OP_LOGIN:
	case ISCSI_OP_TEXT:
		SET_FIELD(wqe->flags, ISCSI_WQE_WQE_TYPE,
			  ISCSI_WQE_TYPE_MIDDLE_PATH);
		SET_FIELD(wqe->flags, ISCSI_WQE_NUM_FAST_SGES,
			  1);
		cont_field->contlen_cdbsize_field = ntoh24(login_hdr->dlength);
		break;
	case ISCSI_OP_LOGOUT:
	case ISCSI_OP_NOOP_OUT:
	case ISCSI_OP_SCSI_TMFUNC:
		 SET_FIELD(wqe->flags, ISCSI_WQE_WQE_TYPE,
			   ISCSI_WQE_TYPE_NORMAL);
		break;
	default:
		if (!sc)
			break;

		SET_FIELD(wqe->flags, ISCSI_WQE_WQE_TYPE,
			  ISCSI_WQE_TYPE_NORMAL);
		cont_field->contlen_cdbsize_field =
				(sc->sc_data_direction == DMA_TO_DEVICE) ?
				scsi_bufflen(sc) : 0;
		if (cmd->use_slowpath)
			SET_FIELD(wqe->flags, ISCSI_WQE_NUM_FAST_SGES, 0);
		else
			SET_FIELD(wqe->flags, ISCSI_WQE_NUM_FAST_SGES,
				  (sc->sc_data_direction ==
				   DMA_TO_DEVICE) ?
				  min((u16)QEDI_FAST_SGE_COUNT,
				      (u16)cmd->io_tbl.sge_valid) : 0);
		break;
	}

	wqe->task_id = tid;
	/* Make sure SQ data is coherent */
	wmb();
}

static void qedi_ring_doorbell(struct qedi_conn *qedi_conn)
{
	struct iscsi_db_data dbell = { 0 };

	dbell.agg_flags = 0;

	dbell.params |= DB_DEST_XCM << ISCSI_DB_DATA_DEST_SHIFT;
	dbell.params |= DB_AGG_CMD_SET << ISCSI_DB_DATA_AGG_CMD_SHIFT;
	dbell.params |=
		   DQ_XCM_ISCSI_SQ_PROD_CMD << ISCSI_DB_DATA_AGG_VAL_SEL_SHIFT;

	dbell.sq_prod = qedi_conn->ep->fw_sq_prod_idx;
	writel(*(u32 *)&dbell, qedi_conn->ep->p_doorbell);

	/* Make sure fw write idx is coherent, and include both memory barriers
	 * as a failsafe as for some architectures the call is the same but on
	 * others they are two different assembly operations.
	 */
	wmb();
	mmiowb();
	QEDI_INFO(&qedi_conn->qedi->dbg_ctx, QEDI_LOG_MP_REQ,
		  "prod_idx=0x%x, fw_prod_idx=0x%x, cid=0x%x\n",
		  qedi_conn->ep->sq_prod_idx, qedi_conn->ep->fw_sq_prod_idx,
		  qedi_conn->iscsi_conn_id);
}

int qedi_send_iscsi_login(struct qedi_conn *qedi_conn,
			  struct iscsi_task *task)
{
	struct qedi_ctx *qedi = qedi_conn->qedi;
	struct iscsi_task_context *fw_task_ctx;
	struct iscsi_login_req *login_hdr;
	struct iscsi_login_req_hdr *fw_login_req = NULL;
	struct iscsi_cached_sge_ctx *cached_sge = NULL;
	struct iscsi_sge *single_sge = NULL;
	struct iscsi_sge *req_sge = NULL;
	struct iscsi_sge *resp_sge = NULL;
	struct qedi_cmd *qedi_cmd;
	s16 ptu_invalidate = 0;
	s16 tid = 0;

	req_sge = (struct iscsi_sge *)qedi_conn->gen_pdu.req_bd_tbl;
	resp_sge = (struct iscsi_sge *)qedi_conn->gen_pdu.resp_bd_tbl;
	qedi_cmd = (struct qedi_cmd *)task->dd_data;
	login_hdr = (struct iscsi_login_req *)task->hdr;

	tid = qedi_get_task_idx(qedi);
	if (tid == -1)
		return -ENOMEM;

	fw_task_ctx = qedi_get_task_mem(&qedi->tasks, tid);
	memset(fw_task_ctx, 0, sizeof(struct iscsi_task_context));

	qedi_cmd->task_id = tid;

	/* Ystorm context */
	fw_login_req = &fw_task_ctx->ystorm_st_context.pdu_hdr.login_req;
	fw_login_req->opcode = login_hdr->opcode;
	fw_login_req->version_min = login_hdr->min_version;
	fw_login_req->version_max = login_hdr->max_version;
	fw_login_req->flags_attr = login_hdr->flags;
	fw_login_req->isid_tabc = *((u16 *)login_hdr->isid + 2);
	fw_login_req->isid_d = *((u32 *)login_hdr->isid);
	fw_login_req->tsih = login_hdr->tsih;
	qedi_update_itt_map(qedi, tid, task->itt, qedi_cmd);
	fw_login_req->itt = qedi_set_itt(tid, get_itt(task->itt));
	fw_login_req->cid = qedi_conn->iscsi_conn_id;
	fw_login_req->cmd_sn = be32_to_cpu(login_hdr->cmdsn);
	fw_login_req->exp_stat_sn = be32_to_cpu(login_hdr->exp_statsn);
	fw_login_req->exp_stat_sn = 0;

	if (qedi->tid_reuse_count[tid] == QEDI_MAX_TASK_NUM) {
		ptu_invalidate = 1;
		qedi->tid_reuse_count[tid] = 0;
	}

	fw_task_ctx->ystorm_st_context.state.reuse_count =
						qedi->tid_reuse_count[tid];
	fw_task_ctx->mstorm_st_context.reuse_count =
						qedi->tid_reuse_count[tid]++;
	cached_sge =
	       &fw_task_ctx->ystorm_st_context.state.sgl_ctx_union.cached_sge;
	cached_sge->sge.sge_len = req_sge->sge_len;
	cached_sge->sge.sge_addr.lo = (u32)(qedi_conn->gen_pdu.req_dma_addr);
	cached_sge->sge.sge_addr.hi =
			     (u32)((u64)qedi_conn->gen_pdu.req_dma_addr >> 32);

	/* Mstorm context */
	single_sge = &fw_task_ctx->mstorm_st_context.sgl_union.single_sge;
	fw_task_ctx->mstorm_st_context.task_type = 0x2;
	fw_task_ctx->mstorm_ag_context.task_cid = (u16)qedi_conn->iscsi_conn_id;
	single_sge->sge_addr.lo = resp_sge->sge_addr.lo;
	single_sge->sge_addr.hi = resp_sge->sge_addr.hi;
	single_sge->sge_len = resp_sge->sge_len;

	SET_FIELD(fw_task_ctx->mstorm_st_context.flags.mflags,
		  ISCSI_MFLAGS_SINGLE_SGE, 1);
	SET_FIELD(fw_task_ctx->mstorm_st_context.flags.mflags,
		  ISCSI_MFLAGS_SLOW_IO, 0);
	fw_task_ctx->mstorm_st_context.sgl_size = 1;
	fw_task_ctx->mstorm_st_context.rem_task_size = resp_sge->sge_len;

	/* Ustorm context */
	fw_task_ctx->ustorm_st_context.rem_rcv_len = resp_sge->sge_len;
	fw_task_ctx->ustorm_st_context.exp_data_transfer_len =
						ntoh24(login_hdr->dlength);
	fw_task_ctx->ustorm_st_context.exp_data_sn = 0;
	fw_task_ctx->ustorm_st_context.cq_rss_number = 0;
	fw_task_ctx->ustorm_st_context.task_type = 0x2;
	fw_task_ctx->ustorm_ag_context.icid = (u16)qedi_conn->iscsi_conn_id;
	fw_task_ctx->ustorm_ag_context.exp_data_acked =
						 ntoh24(login_hdr->dlength);
	SET_FIELD(fw_task_ctx->ustorm_ag_context.flags1,
		  USTORM_ISCSI_TASK_AG_CTX_R2T2RECV, 1);
	SET_FIELD(fw_task_ctx->ustorm_st_context.flags,
		  USTORM_ISCSI_TASK_ST_CTX_LOCAL_COMP, 0);

	spin_lock(&qedi_conn->list_lock);
	list_add_tail(&qedi_cmd->io_cmd, &qedi_conn->active_cmd_list);
	qedi_cmd->io_cmd_in_list = true;
	qedi_conn->active_cmd_count++;
	spin_unlock(&qedi_conn->list_lock);

	qedi_add_to_sq(qedi_conn, task, tid, ptu_invalidate, false);
	qedi_ring_doorbell(qedi_conn);
	return 0;
}

int qedi_send_iscsi_logout(struct qedi_conn *qedi_conn,
			   struct iscsi_task *task)
{
	struct qedi_ctx *qedi = qedi_conn->qedi;
	struct iscsi_logout_req_hdr *fw_logout_req = NULL;
	struct iscsi_task_context *fw_task_ctx = NULL;
	struct iscsi_logout *logout_hdr = NULL;
	struct qedi_cmd *qedi_cmd = NULL;
	s16  tid = 0;
	s16 ptu_invalidate = 0;

	qedi_cmd = (struct qedi_cmd *)task->dd_data;
	logout_hdr = (struct iscsi_logout *)task->hdr;

	tid = qedi_get_task_idx(qedi);
	if (tid == -1)
		return -ENOMEM;

	fw_task_ctx = qedi_get_task_mem(&qedi->tasks, tid);

	memset(fw_task_ctx, 0, sizeof(struct iscsi_task_context));
	qedi_cmd->task_id = tid;

	/* Ystorm context */
	fw_logout_req = &fw_task_ctx->ystorm_st_context.pdu_hdr.logout_req;
	fw_logout_req->opcode = ISCSI_OPCODE_LOGOUT_REQUEST;
	fw_logout_req->reason_code = 0x80 | logout_hdr->flags;
	qedi_update_itt_map(qedi, tid, task->itt, qedi_cmd);
	fw_logout_req->itt = qedi_set_itt(tid, get_itt(task->itt));
	fw_logout_req->exp_stat_sn = be32_to_cpu(logout_hdr->exp_statsn);
	fw_logout_req->cmd_sn = be32_to_cpu(logout_hdr->cmdsn);

	if (qedi->tid_reuse_count[tid] == QEDI_MAX_TASK_NUM) {
		ptu_invalidate = 1;
		qedi->tid_reuse_count[tid] = 0;
	}
	fw_task_ctx->ystorm_st_context.state.reuse_count =
						  qedi->tid_reuse_count[tid];
	fw_task_ctx->mstorm_st_context.reuse_count =
						qedi->tid_reuse_count[tid]++;
	fw_logout_req->cid = qedi_conn->iscsi_conn_id;
	fw_task_ctx->ystorm_st_context.state.buffer_offset[0] = 0;

	/* Mstorm context */
	fw_task_ctx->mstorm_st_context.task_type = ISCSI_TASK_TYPE_MIDPATH;
	fw_task_ctx->mstorm_ag_context.task_cid = (u16)qedi_conn->iscsi_conn_id;

	/* Ustorm context */
	fw_task_ctx->ustorm_st_context.rem_rcv_len = 0;
	fw_task_ctx->ustorm_st_context.exp_data_transfer_len = 0;
	fw_task_ctx->ustorm_st_context.exp_data_sn = 0;
	fw_task_ctx->ustorm_st_context.task_type =  ISCSI_TASK_TYPE_MIDPATH;
	fw_task_ctx->ustorm_st_context.cq_rss_number = 0;

	SET_FIELD(fw_task_ctx->ustorm_st_context.flags,
		  USTORM_ISCSI_TASK_ST_CTX_LOCAL_COMP, 0);
	SET_FIELD(fw_task_ctx->ustorm_st_context.reg1.reg1_map,
		  ISCSI_REG1_NUM_FAST_SGES, 0);

	fw_task_ctx->ustorm_ag_context.icid = (u16)qedi_conn->iscsi_conn_id;
	SET_FIELD(fw_task_ctx->ustorm_ag_context.flags1,
		  USTORM_ISCSI_TASK_AG_CTX_R2T2RECV, 1);

	spin_lock(&qedi_conn->list_lock);
	list_add_tail(&qedi_cmd->io_cmd, &qedi_conn->active_cmd_list);
	qedi_cmd->io_cmd_in_list = true;
	qedi_conn->active_cmd_count++;
	spin_unlock(&qedi_conn->list_lock);

	qedi_add_to_sq(qedi_conn, task, tid, ptu_invalidate, false);
	qedi_ring_doorbell(qedi_conn);

	return 0;
}

int qedi_send_iscsi_text(struct qedi_conn *qedi_conn,
			 struct iscsi_task *task)
{
	struct qedi_ctx *qedi = qedi_conn->qedi;
	struct iscsi_task_context *fw_task_ctx;
	struct iscsi_text_request_hdr *fw_text_request;
	struct iscsi_cached_sge_ctx *cached_sge;
	struct iscsi_sge *single_sge;
	struct qedi_cmd *qedi_cmd;
	/* For 6.5 hdr iscsi_hdr */
	struct iscsi_text *text_hdr;
	struct iscsi_sge *req_sge;
	struct iscsi_sge *resp_sge;
	s16 ptu_invalidate = 0;
	s16 tid = 0;

	req_sge = (struct iscsi_sge *)qedi_conn->gen_pdu.req_bd_tbl;
	resp_sge = (struct iscsi_sge *)qedi_conn->gen_pdu.resp_bd_tbl;
	qedi_cmd = (struct qedi_cmd *)task->dd_data;
	text_hdr = (struct iscsi_text *)task->hdr;

	tid = qedi_get_task_idx(qedi);
	if (tid == -1)
		return -ENOMEM;

	fw_task_ctx = qedi_get_task_mem(&qedi->tasks, tid);
	memset(fw_task_ctx, 0, sizeof(struct iscsi_task_context));

	qedi_cmd->task_id = tid;

	/* Ystorm context */
	fw_text_request =
			&fw_task_ctx->ystorm_st_context.pdu_hdr.text_request;
	fw_text_request->opcode = text_hdr->opcode;
	fw_text_request->flags_attr = text_hdr->flags;

	qedi_update_itt_map(qedi, tid, task->itt, qedi_cmd);
	fw_text_request->itt = qedi_set_itt(tid, get_itt(task->itt));
	fw_text_request->ttt = text_hdr->ttt;
	fw_text_request->cmd_sn = be32_to_cpu(text_hdr->cmdsn);
	fw_text_request->exp_stat_sn = be32_to_cpu(text_hdr->exp_statsn);
	fw_text_request->hdr_second_dword = ntoh24(text_hdr->dlength);

	if (qedi->tid_reuse_count[tid] == QEDI_MAX_TASK_NUM) {
		ptu_invalidate = 1;
		qedi->tid_reuse_count[tid] = 0;
	}
	fw_task_ctx->ystorm_st_context.state.reuse_count =
						     qedi->tid_reuse_count[tid];
	fw_task_ctx->mstorm_st_context.reuse_count =
						   qedi->tid_reuse_count[tid]++;

	cached_sge =
	       &fw_task_ctx->ystorm_st_context.state.sgl_ctx_union.cached_sge;
	cached_sge->sge.sge_len = req_sge->sge_len;
	cached_sge->sge.sge_addr.lo = (u32)(qedi_conn->gen_pdu.req_dma_addr);
	cached_sge->sge.sge_addr.hi =
			      (u32)((u64)qedi_conn->gen_pdu.req_dma_addr >> 32);

	/* Mstorm context */
	single_sge = &fw_task_ctx->mstorm_st_context.sgl_union.single_sge;
	fw_task_ctx->mstorm_st_context.task_type = 0x2;
	fw_task_ctx->mstorm_ag_context.task_cid = (u16)qedi_conn->iscsi_conn_id;
	single_sge->sge_addr.lo = resp_sge->sge_addr.lo;
	single_sge->sge_addr.hi = resp_sge->sge_addr.hi;
	single_sge->sge_len = resp_sge->sge_len;

	SET_FIELD(fw_task_ctx->mstorm_st_context.flags.mflags,
		  ISCSI_MFLAGS_SINGLE_SGE, 1);
	SET_FIELD(fw_task_ctx->mstorm_st_context.flags.mflags,
		  ISCSI_MFLAGS_SLOW_IO, 0);
	fw_task_ctx->mstorm_st_context.sgl_size = 1;
	fw_task_ctx->mstorm_st_context.rem_task_size = resp_sge->sge_len;

	/* Ustorm context */
	fw_task_ctx->ustorm_ag_context.exp_data_acked =
						      ntoh24(text_hdr->dlength);
	fw_task_ctx->ustorm_st_context.rem_rcv_len = resp_sge->sge_len;
	fw_task_ctx->ustorm_st_context.exp_data_transfer_len =
						      ntoh24(text_hdr->dlength);
	fw_task_ctx->ustorm_st_context.exp_data_sn =
					      be32_to_cpu(text_hdr->exp_statsn);
	fw_task_ctx->ustorm_st_context.cq_rss_number = 0;
	fw_task_ctx->ustorm_st_context.task_type = 0x2;
	fw_task_ctx->ustorm_ag_context.icid = (u16)qedi_conn->iscsi_conn_id;
	SET_FIELD(fw_task_ctx->ustorm_ag_context.flags1,
		  USTORM_ISCSI_TASK_AG_CTX_R2T2RECV, 1);

	/*  Add command in active command list */
	spin_lock(&qedi_conn->list_lock);
	list_add_tail(&qedi_cmd->io_cmd, &qedi_conn->active_cmd_list);
	qedi_cmd->io_cmd_in_list = true;
	qedi_conn->active_cmd_count++;
	spin_unlock(&qedi_conn->list_lock);

	qedi_add_to_sq(qedi_conn, task, tid, ptu_invalidate, false);
	qedi_ring_doorbell(qedi_conn);

	return 0;
}

int qedi_send_iscsi_nopout(struct qedi_conn *qedi_conn,
			   struct iscsi_task *task,
			   char *datap, int data_len, int unsol)
{
	struct qedi_ctx *qedi = qedi_conn->qedi;
	struct iscsi_task_context *fw_task_ctx;
	struct iscsi_nop_out_hdr *fw_nop_out;
	struct qedi_cmd *qedi_cmd;
	/* For 6.5 hdr iscsi_hdr */
	struct iscsi_nopout *nopout_hdr;
	struct iscsi_cached_sge_ctx *cached_sge;
	struct iscsi_sge *single_sge;
	struct iscsi_sge *req_sge;
	struct iscsi_sge *resp_sge;
	u32 lun[2];
	s16 ptu_invalidate = 0;
	s16 tid = 0;

	req_sge = (struct iscsi_sge *)qedi_conn->gen_pdu.req_bd_tbl;
	resp_sge = (struct iscsi_sge *)qedi_conn->gen_pdu.resp_bd_tbl;
	qedi_cmd = (struct qedi_cmd *)task->dd_data;
	nopout_hdr = (struct iscsi_nopout *)task->hdr;

	tid = qedi_get_task_idx(qedi);
	if (tid == -1) {
		QEDI_WARN(&qedi->dbg_ctx, "Invalid tid\n");
		return -ENOMEM;
	}

	fw_task_ctx = qedi_get_task_mem(&qedi->tasks, tid);

	memset(fw_task_ctx, 0, sizeof(struct iscsi_task_context));
	qedi_cmd->task_id = tid;

	/* Ystorm context */
	fw_nop_out = &fw_task_ctx->ystorm_st_context.pdu_hdr.nop_out;
	SET_FIELD(fw_nop_out->flags_attr, ISCSI_NOP_OUT_HDR_CONST1, 1);
	SET_FIELD(fw_nop_out->flags_attr, ISCSI_NOP_OUT_HDR_RSRV, 0);

	memcpy(lun, &nopout_hdr->lun, sizeof(struct scsi_lun));
	fw_nop_out->lun.lo = be32_to_cpu(lun[0]);
	fw_nop_out->lun.hi = be32_to_cpu(lun[1]);

	qedi_update_itt_map(qedi, tid, task->itt, qedi_cmd);

	if (nopout_hdr->ttt != ISCSI_TTT_ALL_ONES) {
		fw_nop_out->itt = be32_to_cpu(nopout_hdr->itt);
		fw_nop_out->ttt = be32_to_cpu(nopout_hdr->ttt);
		fw_task_ctx->ystorm_st_context.state.buffer_offset[0] = 0;
		fw_task_ctx->ystorm_st_context.state.local_comp = 1;
		SET_FIELD(fw_task_ctx->ustorm_st_context.flags,
			  USTORM_ISCSI_TASK_ST_CTX_LOCAL_COMP, 1);
	} else {
		fw_nop_out->itt = qedi_set_itt(tid, get_itt(task->itt));
		fw_nop_out->ttt = ISCSI_TTT_ALL_ONES;
		fw_task_ctx->ystorm_st_context.state.buffer_offset[0] = 0;

		spin_lock(&qedi_conn->list_lock);
		list_add_tail(&qedi_cmd->io_cmd, &qedi_conn->active_cmd_list);
		qedi_cmd->io_cmd_in_list = true;
		qedi_conn->active_cmd_count++;
		spin_unlock(&qedi_conn->list_lock);
	}

	fw_nop_out->opcode = ISCSI_OPCODE_NOP_OUT;
	fw_nop_out->cmd_sn = be32_to_cpu(nopout_hdr->cmdsn);
	fw_nop_out->exp_stat_sn = be32_to_cpu(nopout_hdr->exp_statsn);

	cached_sge =
	       &fw_task_ctx->ystorm_st_context.state.sgl_ctx_union.cached_sge;
	cached_sge->sge.sge_len = req_sge->sge_len;
	cached_sge->sge.sge_addr.lo = (u32)(qedi_conn->gen_pdu.req_dma_addr);
	cached_sge->sge.sge_addr.hi =
			(u32)((u64)qedi_conn->gen_pdu.req_dma_addr >> 32);

	/* Mstorm context */
	fw_task_ctx->mstorm_st_context.task_type = ISCSI_TASK_TYPE_MIDPATH;
	fw_task_ctx->mstorm_ag_context.task_cid = (u16)qedi_conn->iscsi_conn_id;

	single_sge = &fw_task_ctx->mstorm_st_context.sgl_union.single_sge;
	single_sge->sge_addr.lo = resp_sge->sge_addr.lo;
	single_sge->sge_addr.hi = resp_sge->sge_addr.hi;
	single_sge->sge_len = resp_sge->sge_len;
	fw_task_ctx->mstorm_st_context.rem_task_size = resp_sge->sge_len;

	if (qedi->tid_reuse_count[tid] == QEDI_MAX_TASK_NUM) {
		ptu_invalidate = 1;
		qedi->tid_reuse_count[tid] = 0;
	}
	fw_task_ctx->ystorm_st_context.state.reuse_count =
						qedi->tid_reuse_count[tid];
	fw_task_ctx->mstorm_st_context.reuse_count =
						qedi->tid_reuse_count[tid]++;
	/* Ustorm context */
	fw_task_ctx->ustorm_st_context.rem_rcv_len = resp_sge->sge_len;
	fw_task_ctx->ustorm_st_context.exp_data_transfer_len = data_len;
	fw_task_ctx->ustorm_st_context.exp_data_sn = 0;
	fw_task_ctx->ustorm_st_context.task_type =  ISCSI_TASK_TYPE_MIDPATH;
	fw_task_ctx->ustorm_st_context.cq_rss_number = 0;

	SET_FIELD(fw_task_ctx->ustorm_st_context.reg1.reg1_map,
		  ISCSI_REG1_NUM_FAST_SGES, 0);

	fw_task_ctx->ustorm_ag_context.icid = (u16)qedi_conn->iscsi_conn_id;
	SET_FIELD(fw_task_ctx->ustorm_ag_context.flags1,
		  USTORM_ISCSI_TASK_AG_CTX_R2T2RECV, 1);

	fw_task_ctx->ustorm_st_context.lun.lo = be32_to_cpu(lun[0]);
	fw_task_ctx->ustorm_st_context.lun.hi = be32_to_cpu(lun[1]);

	qedi_add_to_sq(qedi_conn, task, tid, ptu_invalidate, false);
	qedi_ring_doorbell(qedi_conn);
	return 0;
}
