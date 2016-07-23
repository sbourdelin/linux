/*******************************************************************
 * This file is part of the Emulex Linux Device Driver for         *
 * Fibre Channel Host Bus Adapters.                                *
 * Copyright (C) 2004-2016 Emulex.  All rights reserved.           *
 * EMULEX and SLI are trademarks of Emulex.                        *
 * www.emulex.com                                                  *
 * Portions Copyright (C) 2004-2005 Christoph Hellwig              *
 *                                                                 *
 * This program is free software; you can redistribute it and/or   *
 * modify it under the terms of version 2 of the GNU General       *
 * Public License as published by the Free Software Foundation.    *
 * This program is distributed in the hope that it will be useful. *
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND          *
 * WARRANTIES, INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY,  *
 * FITNESS FOR A PARTICULAR PURPOSE, OR NON-INFRINGEMENT, ARE      *
 * DISCLAIMED, EXCEPT TO THE EXTENT THAT SUCH DISCLAIMERS ARE HELD *
 * TO BE LEGALLY INVALID.  See the GNU General Public License for  *
 * more details, a copy of which can be found in the file COPYING  *
 * included with this package.                                     *
 ********************************************************************/
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <asm/unaligned.h>
#include <linux/crc-t10dif.h>
#include <net/checksum.h>

#include <scsi/scsi.h>
#include <scsi/scsi_device.h>
#include <scsi/scsi_eh.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_tcq.h>
#include <scsi/scsi_transport_fc.h>
#include <scsi/fc/fc_fs.h>

#include <linux/nvme-fc-driver.h>

#include "lpfc_version.h"
#include "lpfc_hw4.h"
#include "lpfc_hw.h"
#include "lpfc_sli.h"
#include "lpfc_sli4.h"
#include "lpfc_nl.h"
#include "lpfc_disc.h"
#include "lpfc_nvme.h"
#include "lpfc.h"
#include "lpfc_scsi.h"
#include "lpfc_logmsg.h"
#include "lpfc_crtn.h"
#include "lpfc_vport.h"

/* NVME initiator-based functions */

/**
 * lpfc_nvme_create_hw_queue -
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @qidx: An cpu index used to affinitize IO queues and MSIX vectors.
 * @handle: An opaque driver handle used in follow-up calls.
 *
 * Driver registers this routine to preallocate and initialize
 * any internal data structures to bind the @qidx to its internal
 * IO queues.
 *
 * Return value :
 *   0 - Success
 *   TODO:  What are the failure codes.
 **/
static int
lpfc_nvme_create_hw_queue(struct nvme_fc_local_port *pnvme_lport,
		       unsigned int qnum, u16 qsize,
		       void **handle)
{
	uint32_t cpu = 0;
	uint32_t qidx;
	struct lpfc_nvme_lport *lport;
	struct lpfc_vport *vport;
	struct lpfc_nvme_qhandle *qhandle;

	lport = (struct lpfc_nvme_lport *) pnvme_lport->private;
	vport = lport->pnvme->vport;
	qidx = 0;  /* Hardcode for now */

	lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
			 "6000 ENTER.  lpfc_pnvme %p, qidx x%x running "
			 "cpu %d\n",
			 lport, qidx, smp_processor_id());

	/* Display all online CPUs. */
	for_each_present_cpu(cpu) {
		if (cpu_online(cpu)) {
			lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
					 "9999 CPU %d online\n",
					 cpu);
			if (cpu == qidx) {
				qhandle = kzalloc(
					  sizeof(struct lpfc_nvme_qhandle),
					  GFP_KERNEL);
				if (qhandle == NULL)
					return -ENOMEM;

				qhandle->cpu_id = qidx;
				qhandle->wq_id = vport->last_fcp_wqidx;
				vport->last_fcp_wqidx =
					(vport->last_fcp_wqidx + 1) %
					vport->phba->cfg_nvme_io_channel;
				lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
						 "6073 Binding qidx %d to "
						 "fcp_wqidx %d in qhandle %p\n",
						 qidx, vport->last_fcp_wqidx,
						 qhandle);
				handle = (void **)&qhandle;
				return 0;
			}
		} else
			lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
					 "9999 CPU %d offline\n",
					 cpu);
	}

	/* Stub in routine and return 0 for now. */
	return -EINVAL;
}

/**
 * lpfc_nvme_delete_hw_queue -
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @qidx: An cpu index used to affinitize IO queues and MSIX vectors.
 * @handle: An opaque driver handle from lpfc_nvme_create_hw_queue
 *
 * Driver registers this routine to free
 * any internal data structures to bind the @qidx to its internal
 * IO queues.
 *
 * Return value :
 *   0 - Success
 *   TODO:  What are the failure codes.
 **/
static void
lpfc_nvme_delete_hw_queue(struct nvme_fc_local_port *pnvme_lport,
		       unsigned int qidx,
		       void *handle)
{
	struct lpfc_nvme_lport *lport;
	struct lpfc_vport *vport;

	lport = (struct lpfc_nvme_lport *) pnvme_lport->private;
	vport = lport->pnvme->vport;

	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME,
			"6001 ENTER.  lpfc_pnvme %p, qidx x%xi qhandle %p\n",
			lport, qidx, handle);
	kfree(handle);
}

static void
lpfc_nvme_cmpl_gen_req(struct lpfc_hba *phba, struct lpfc_iocbq *cmdwqe,
		       struct lpfc_wcqe_complete *wcqe)
{
	struct lpfc_vport *vport = cmdwqe->vport;
	uint32_t status;
	struct nvmefc_ls_req *pnvme_lsreq;
	struct lpfc_dmabuf *buf_ptr;
	struct lpfc_nodelist *ndlp;

	pnvme_lsreq = (struct nvmefc_ls_req *)cmdwqe->context2;
	status = bf_get(lpfc_wcqe_c_status, wcqe) & LPFC_IOCB_STATUS_MASK;
	ndlp = (struct lpfc_nodelist *)cmdwqe->context1;
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME,
			 "6047 nvme cmpl Enter "
			 "Data %p DID %x Xri: %x status %x cmd:%p lsreg:%p "
			 "bmp:%p ndlp:%p\n",
			 pnvme_lsreq, ndlp ? ndlp->nlp_DID : 0,
			 cmdwqe->sli4_xritag, status,
			 cmdwqe, pnvme_lsreq, cmdwqe->context3, ndlp);

	if (cmdwqe->context3) {
		buf_ptr = (struct lpfc_dmabuf *)cmdwqe->context3;
		lpfc_mbuf_free(phba, buf_ptr->virt, buf_ptr->phys);
		kfree(buf_ptr);
		cmdwqe->context3 = NULL;
	}
	if (pnvme_lsreq->done)
		pnvme_lsreq->done(pnvme_lsreq, status);
	else
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
				 "6046 nvme cmpl without done call back? "
				 "Data %p DID %x Xri: %x status %x\n",
				pnvme_lsreq, ndlp ? ndlp->nlp_DID : 0,
				cmdwqe->sli4_xritag, status);
	if (ndlp) {
		lpfc_nlp_put(ndlp);
		cmdwqe->context1 = NULL;
	}
	lpfc_sli_release_iocbq(phba, cmdwqe);
}

static int
lpfc_nvme_gen_req(struct lpfc_vport *vport, struct lpfc_dmabuf *bmp,
		  struct lpfc_dmabuf *inp,
		 struct nvmefc_ls_req *pnvme_lsreq,
	     void (*cmpl)(struct lpfc_hba *, struct lpfc_iocbq *,
			   struct lpfc_wcqe_complete *),
	     struct lpfc_nodelist *ndlp, uint32_t num_entry,
	     uint32_t tmo, uint8_t retry)
{
	struct lpfc_hba  *phba = vport->phba;
	union lpfc_wqe *wqe;
	struct lpfc_iocbq *genwqe;
	struct ulp_bde64 *bpl;
	struct ulp_bde64 bde;
	int i, rc, xmit_len, first_len;

	/* Allocate buffer for  command WQE */
	genwqe = lpfc_sli_get_iocbq(phba);
	if (genwqe == NULL)
		return 1;

	wqe = &genwqe->wqe;
	memset(wqe, 0, sizeof(union lpfc_wqe));

	genwqe->context3 = (uint8_t *)bmp;
	genwqe->iocb_flag |= LPFC_IO_NVME_LS;

	/* Save for completion so we can release these resources */
	genwqe->context1 = lpfc_nlp_get(ndlp);
	genwqe->context2 = (uint8_t *)pnvme_lsreq;
	/* Fill in payload, bp points to frame payload */

	if (!tmo)
		/* FC spec states we need 3 * ratov for CT requests */
		tmo = (3 * phba->fc_ratov);

	/* For this command calculate the xmit length of the request bde. */
	xmit_len = 0;
	first_len = 0;
	bpl = (struct ulp_bde64 *)bmp->virt;
	for (i = 0; i < num_entry; i++) {
		bde.tus.w = bpl[i].tus.w;
		if (bde.tus.f.bdeFlags != BUFF_TYPE_BDE_64)
			break;
		xmit_len += bde.tus.f.bdeSize;
		if (i == 0)
			first_len = xmit_len;
	}

	genwqe->rsvd2 = num_entry;
	genwqe->hba_wqidx = 0;

	/* Words 0 - 2 */
	wqe->generic.bde.tus.f.bdeFlags = BUFF_TYPE_BDE_64;
	wqe->generic.bde.tus.f.bdeSize = first_len;
	wqe->generic.bde.addrLow = bpl[0].addrLow;
	wqe->generic.bde.addrHigh = bpl[0].addrHigh;

	/* Word 3 */
	wqe->gen_req.request_payload_len = first_len;

	/* Word 4 */

	/* Word 5 */
	bf_set(wqe_dfctl, &wqe->gen_req.wge_ctl, 0);
	bf_set(wqe_si, &wqe->gen_req.wge_ctl, 1);
	bf_set(wqe_la, &wqe->gen_req.wge_ctl, 1);
	bf_set(wqe_rctl, &wqe->gen_req.wge_ctl, FC_RCTL_DD_UNSOL_CTL);
	bf_set(wqe_type, &wqe->gen_req.wge_ctl, LPFC_FC4_TYPE_NVME);

	/* Word 6 */
	bf_set(wqe_ctxt_tag, &wqe->gen_req.wqe_com,
	       phba->sli4_hba.rpi_ids[ndlp->nlp_rpi]);
	bf_set(wqe_xri_tag, &wqe->gen_req.wqe_com, genwqe->sli4_xritag);

	/* Word 7 */
	bf_set(wqe_tmo, &wqe->gen_req.wqe_com, (vport->phba->fc_ratov-1));
	bf_set(wqe_class, &wqe->gen_req.wqe_com, CLASS3);
	bf_set(wqe_cmnd, &wqe->gen_req.wqe_com, CMD_GEN_REQUEST64_WQE);
	bf_set(wqe_ct, &wqe->gen_req.wqe_com, SLI4_CT_RPI);

	/* Word 8 */
	wqe->gen_req.wqe_com.abort_tag = genwqe->iotag;

	/* Word 9 */
	bf_set(wqe_reqtag, &wqe->gen_req.wqe_com, genwqe->iotag);

	/* Word 10 */
	bf_set(wqe_dbde, &wqe->gen_req.wqe_com, 1);
	bf_set(wqe_iod, &wqe->gen_req.wqe_com, LPFC_WQE_IOD_READ);
	bf_set(wqe_qosd, &wqe->gen_req.wqe_com, 1);
	bf_set(wqe_lenloc, &wqe->gen_req.wqe_com, LPFC_WQE_LENLOC_NONE);
	bf_set(wqe_ebde_cnt, &wqe->gen_req.wqe_com, 0);

	/* Word 11 */
	bf_set(wqe_cqid, &wqe->gen_req.wqe_com, LPFC_WQE_CQ_ID_DEFAULT);
	bf_set(wqe_cmd_type, &wqe->gen_req.wqe_com, OTHER_COMMAND);


	/* Issue GEN REQ WQE for NPORT <did> */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_ELS,
			 "6050 Issue GEN REQ WQE to NPORT x%x "
			 "Data: x%x x%x wq:%p lsreq:%p bmp:%p xmit:%d 1st:%d\n",
			 ndlp->nlp_DID, genwqe->iotag,
			 vport->port_state,
			genwqe, pnvme_lsreq, bmp, xmit_len, first_len);
	genwqe->wqe_cmpl = cmpl;
	genwqe->iocb_cmpl = NULL;
	genwqe->drvrTimeout = tmo + LPFC_DRVR_TIMEOUT;
	genwqe->vport = vport;
	genwqe->retry = retry;

	rc = lpfc_sli_issue_wqe(phba, LPFC_ELS_RING, genwqe);
	if (rc == WQE_ERROR) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_ELS,
				 "6045 Issue GEN REQ WQE to NPORT x%x "
				 "Data: x%x x%x\n",
				 ndlp->nlp_DID, genwqe->iotag,
				 vport->port_state);
		lpfc_sli_release_iocbq(phba, genwqe);
		return 1;
	}
	return 0;
}

/**
 * lpfc_nvme_ls_req - Issue an Link Service request
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @lpfc_nvme_lport: Pointer to the driver's local port data
 * @lpfc_nvme_rport: Pointer to the rport getting the @lpfc_nvme_ereq
 *
 * Driver registers this routine to handle any link service request
 * from the nvme_fc transport to a remote nvme-aware port.
 *
 * Return value :
 *   0 - Success
 *   TODO: What are the failure codes.
 **/
static int
lpfc_nvme_ls_req(struct nvme_fc_local_port *pnvme_lport,
		 struct nvme_fc_remote_port *pnvme_rport,
		 struct nvmefc_ls_req *pnvme_lsreq)
{
	int ret = 0;
	struct lpfc_nvme_lport *lport;
	struct lpfc_vport *vport;
	struct lpfc_nodelist *ndlp;
	struct ulp_bde64 *bpl;
	struct lpfc_dmabuf *bmp;

	/* there are two dma buf in the request, actually there is one and
	** the second one is just the start address + cmd size.
	** Before calling lpfc_nvme_gen_req these buffers need to be wrapped
	** in a lpfc_dmabuf struct. When freeing we just free the wrapper
	** because the nvem layer owns the data bufs.
	** We do not have to break these packets open, we don't care what is in
	** them. And we do not have to look at the resonse data, we only care
	** that we got a response. All of the caring is going to happen in the
	** nvme-fc layer.
	*/

	lport = (struct lpfc_nvme_lport *) pnvme_lport->private;
	vport = lport->pnvme->vport;

	ndlp = lpfc_findnode_did(vport, pnvme_rport->port_id);
	if (!ndlp) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
				 "6043 Could not find node for DID %x\n",
				 pnvme_rport->port_id);
		return 1;
	}
	bmp = kmalloc(sizeof(struct lpfc_dmabuf), GFP_KERNEL);
	if (!bmp) {

		lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
				 "6044 Could not find node for DID %x\n",
				 pnvme_rport->port_id);
		return 2;
	}
	INIT_LIST_HEAD(&bmp->list);
	bmp->virt = lpfc_mbuf_alloc(vport->phba, MEM_PRI, &(bmp->phys));
	if (!bmp->virt) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
				 "6042 Could not find node for DID %x\n",
				 pnvme_rport->port_id);
		kfree(bmp);
		return 3;
	}
	bpl = (struct ulp_bde64 *)bmp->virt;
	bpl->addrHigh = le32_to_cpu(putPaddrHigh(pnvme_lsreq->rqstdma));
	bpl->addrLow = le32_to_cpu(putPaddrLow(pnvme_lsreq->rqstdma));
	bpl->tus.f.bdeFlags = 0;
	bpl->tus.f.bdeSize = pnvme_lsreq->rqstlen;
	bpl->tus.w = le32_to_cpu(bpl->tus.w);
	bpl++;

	bpl->addrHigh = le32_to_cpu(putPaddrHigh(pnvme_lsreq->rspdma));
	bpl->addrLow = le32_to_cpu(putPaddrLow(pnvme_lsreq->rspdma));
	bpl->tus.f.bdeFlags = BUFF_TYPE_BDE_64I;
	bpl->tus.f.bdeSize = pnvme_lsreq->rsplen;
	bpl->tus.w = le32_to_cpu(bpl->tus.w);

	/* Expand print to include key fields. */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME,
			 "6051 ENTER.  lport %p, rport %p lsreq%p rqstlen:%d "
			 "rsplen:%d %llux %llux\n",
			 pnvme_lport, pnvme_rport,
			 pnvme_lsreq, pnvme_lsreq->rqstlen,
			 pnvme_lsreq->rsplen, pnvme_lsreq->rqstdma,
			 pnvme_lsreq->rspdma);

	/* Hardcode the wait to 30 seconds.  Connections are failing otherwise.
	 * This code allows it all to work.
	 */
	ret = lpfc_nvme_gen_req(vport, bmp, pnvme_lsreq->rqstaddr,
				pnvme_lsreq, lpfc_nvme_cmpl_gen_req,
				ndlp, 2, 30, 0);
	if (ret != WQE_SUCCESS) {
		lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME,
				 "6052 EXIT. issue ls wqe failed lport %p, "
				 "rport %p lsreq%p Status %x DID %x\n",
				 pnvme_lport, pnvme_rport, pnvme_lsreq,
				 ret, ndlp->nlp_DID);
		lpfc_mbuf_free(vport->phba, bmp->virt, bmp->phys);
		kfree(bmp);
		return ret;
	}

	/* Stub in routine and return 0 for now. */
	return ret;
}

/**
 * lpfc_nvme_ls_abort - Issue an Link Service request
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @lpfc_nvme_lport: Pointer to the driver's local port data
 * @lpfc_nvme_rport: Pointer to the rport getting the @lpfc_nvme_ereq
 *
 * Driver registers this routine to handle any link service request
 * from the nvme_fc transport to a remote nvme-aware port.
 *
 * Return value :
 *   0 - Success
 *   TODO: What are the failure codes.
 **/
static void
lpfc_nvme_ls_abort(struct nvme_fc_local_port *pnvme_lport,
		   struct nvme_fc_remote_port *pnvme_rport,
		   struct nvmefc_ls_req *pnvme_lsreq)
{
	struct lpfc_nvme_lport *lport;
	struct lpfc_vport *vport;
	struct lpfc_nodelist *ndlp;

	lport = (struct lpfc_nvme_lport *) pnvme_lport->private;
	vport = lport->pnvme->vport;

	ndlp = lpfc_findnode_did(vport, pnvme_rport->port_id);
	if (!ndlp) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
				 "6043 Could not find node for DID %x\n",
				 pnvme_rport->port_id);
		return;
	}

	/* Expand print to include key fields. */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME,
			 "6006 ENTER.  lport %p, rport %p lsreq %p rqstlen:%d "
			 "rsplen:%d %llux %llux\n",
			 pnvme_lport, pnvme_rport,
			 pnvme_lsreq, pnvme_lsreq->rqstlen,
			 pnvme_lsreq->rsplen, pnvme_lsreq->rqstdma,
			 pnvme_lsreq->rspdma);
}

/* Fix up the existing sgls for NVME IO. */
static void
lpfc_nvme_adj_fcp_sgls(struct lpfc_vport *vport,
		       struct lpfc_scsi_buf *psb,
		       struct nvmefc_fcp_req *nCmd)
{
	struct sli4_sge *sgl;
	union lpfc_wqe128 *wqe128;
	uint32_t *wptr, *dptr;

	/*
	 * Adjust the FCP_CMD and FCP_RSP DMA data and sge_len to
	 * match NVME.  NVME sends 96 bytes. Also, use the
	 * nvme commands command and response dma addresses
	 * rather than the virtual memory to ease the restore
	 * operation.
	 */
	sgl = (struct sli4_sge *)psb->fcp_bpl;
	sgl->addr_hi = cpu_to_le32(putPaddrHigh(nCmd->cmddma));
	sgl->addr_lo = cpu_to_le32(putPaddrLow(nCmd->cmddma));
	sgl->word2 = le32_to_cpu(sgl->word2);
	bf_set(lpfc_sli4_sge_last, sgl, 0);
	sgl->word2 = cpu_to_le32(sgl->word2);
	sgl->sge_len = cpu_to_le32(nCmd->cmdlen);
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME | LOG_FCP,
			 "6063 Reconfig fcp_cmd to len %d bytes "
			 "from cmddma 0x%llx\n",
			 sgl->sge_len, nCmd->cmddma);
	sgl++;

	/* Setup the physical region for the FCP RSP */
	sgl->addr_hi = cpu_to_le32(putPaddrHigh(nCmd->rspdma));
	sgl->addr_lo = cpu_to_le32(putPaddrLow(nCmd->rspdma));
	sgl->word2 = le32_to_cpu(sgl->word2);
	bf_set(lpfc_sli4_sge_last, sgl, 1);
	sgl->word2 = cpu_to_le32(sgl->word2);
	sgl->sge_len = cpu_to_le32(nCmd->rsplen);
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME | LOG_FCP,
			 "6066 Reconfig fcp_rsp to len %d bytes "
			 "from rspdma 0x%llx\n",
			 sgl->sge_len, nCmd->rspdma);

	/*
	 * Get a local pointer to the built-in wqe and correct
	 * the fcp_cmd size to match NVME's 96 bytes and fix
	 * the dma address.
	 */

	/* 128 byte wqe support here */
	wqe128 = (union lpfc_wqe128 *)&psb->cur_iocbq.wqe;

	/* Word 0-2 - NVME CMND IU (embedded payload) */
	wqe128->generic.bde.tus.f.bdeFlags = BUFF_TYPE_BDE_IMMED;
	wqe128->generic.bde.tus.f.bdeSize = 60;
	wqe128->generic.bde.addrHigh = 0;
	wqe128->generic.bde.addrLow =  64;  /* Word 16 */

	/* Word 10 */
	bf_set(wqe_nvme, &wqe128->fcp_icmd.wqe_com, 1);
	bf_set(wqe_wqes, &wqe128->fcp_icmd.wqe_com, 1);

	/*
	 * Embed the payload in the last half of the WQE
	 * WQE words 16-30 get the NVME CMD IU payload
	 *
	 * WQE words 16-18 get payload Words 4-6
	 * WQE words 19-20 get payload Words 8-9
	 * WQE words 21-30 get payload Words 14-23
	 */
	wptr = &wqe128->words[16];  /* WQE ptr */
	dptr = (uint32_t *)nCmd->cmdaddr;  /* payload ptr */
	dptr += 4;		/* Skip Words 0-3 in payload */
	*wptr++ = *dptr++;	/* Word 4 */
	*wptr++ = *dptr++;	/* Word 5 */
	*wptr++ = *dptr++;	/* Word 6 */
	dptr++;			/* Skip Word 7 in payload */
	*wptr++ = *dptr++;	/* Word 8 */
	*wptr++ = *dptr++;	/* Word 9 */
	dptr += 4;		/* Skip Words 10-13 in payload */
	*wptr++ = *dptr++;	/* Word 14 */
	*wptr++ = *dptr++;	/* Word 15 */
	*wptr++ = *dptr++;	/* Word 16 */
	*wptr++ = *dptr++;	/* Word 17 */
	*wptr++ = *dptr++;	/* Word 18 */
	*wptr++ = *dptr++;	/* Word 19 */
	*wptr++ = *dptr++;	/* Word 20 */
	*wptr++ = *dptr++;	/* Word 21 */
	*wptr++ = *dptr++;	/* Word 22 */
	*wptr = *dptr;		/* Word 23 */
}

/* Restore the psb fcp_cmd and fcp_rsp regions for fcp io. */
static void
lpfc_nvme_restore_fcp_sgls(struct lpfc_vport *vport,
			   struct lpfc_scsi_buf *psb)
{
	struct sli4_sge *sgl;
	dma_addr_t pdma_phys_fcp_cmd;
	dma_addr_t pdma_phys_fcp_rsp;
	dma_addr_t pdma_phys_bpl;
	union lpfc_wqe *wqe;
	int sgl_size;

	sgl_size = vport->phba->cfg_sg_dma_buf_size -
		(sizeof(struct fcp_cmnd) + sizeof(struct fcp_rsp));

	/* Just restore what lpfc_new_scsi_buf setup. */
	psb->fcp_bpl = psb->data;
	psb->fcp_cmnd = (psb->data + sgl_size);
	psb->fcp_rsp = (struct fcp_rsp *)((uint8_t *)psb->fcp_cmnd +
					  sizeof(struct fcp_cmnd));

	/* Initialize local short-hand pointers. */
	sgl = (struct sli4_sge *)psb->fcp_bpl;
	pdma_phys_bpl = psb->dma_handle;
	pdma_phys_fcp_cmd = (psb->dma_handle + sgl_size);
	pdma_phys_fcp_rsp = pdma_phys_fcp_cmd + sizeof(struct fcp_cmnd);

	/*
	 * The first two bdes are the FCP_CMD and FCP_RSP.
	 * The balance are sg list bdes. Initialize the
	 * first two and leave the rest for queuecommand.
	 */
	sgl->addr_hi = cpu_to_le32(putPaddrHigh(pdma_phys_fcp_cmd));
	sgl->addr_lo = cpu_to_le32(putPaddrLow(pdma_phys_fcp_cmd));
	sgl->word2 = le32_to_cpu(sgl->word2);
	bf_set(lpfc_sli4_sge_last, sgl, 0);
	sgl->word2 = cpu_to_le32(sgl->word2);
	sgl->sge_len = cpu_to_le32(sizeof(struct fcp_cmnd));
	sgl++;

	/* Setup the physical region for the FCP RSP */
	sgl->addr_hi = cpu_to_le32(putPaddrHigh(pdma_phys_fcp_rsp));
	sgl->addr_lo = cpu_to_le32(putPaddrLow(pdma_phys_fcp_rsp));
	sgl->word2 = le32_to_cpu(sgl->word2);
	bf_set(lpfc_sli4_sge_last, sgl, 1);
	sgl->word2 = cpu_to_le32(sgl->word2);
	sgl->sge_len = cpu_to_le32(sizeof(struct fcp_rsp));

	/*
	 * Get a local pointer to the built-in wqe and correct
	 * the fcp_cmd size to match NVME's 96 bytes and fix
	 * the dma address.
	 */
	wqe = &psb->cur_iocbq.wqe;
	wqe->generic.bde.tus.f.bdeSize = sizeof(struct fcp_cmnd);
	wqe->generic.bde.addrLow = putPaddrLow(pdma_phys_fcp_cmd);
	wqe->generic.bde.addrHigh = putPaddrHigh(pdma_phys_fcp_cmd);
}

/**
 * lpfc_nvme_io_cmd_wqe_cmpl - Complete an NVME-over-FCP IO
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @lpfc_nvme_lport: Pointer to the driver's local port data
 * @lpfc_nvme_rport: Pointer to the rport getting the @lpfc_nvme_ereq
 *
 * Driver registers this routine as it io request handler.  This
 * routine issues an fcp WQE with data from the @lpfc_nvme_fcpreq
 * data structure to the rport indicated in @lpfc_nvme_rport.
 *
 * Return value :
 *   0 - Success
 *   TODO: What are the failure codes.
 **/
static void
lpfc_nvme_io_cmd_wqe_cmpl(struct lpfc_hba *phba, struct lpfc_iocbq *pwqeIn,
			  struct lpfc_wcqe_complete *wcqe)
{
	struct lpfc_scsi_buf *lpfc_cmd =
		(struct lpfc_scsi_buf *)pwqeIn->context1;
	struct lpfc_vport *vport = pwqeIn->vport;
	struct nvmefc_fcp_req *nCmd;
	struct lpfc_nvme_rport *rport;
	struct lpfc_nodelist *ndlp;
	unsigned long flags;
	uint32_t code, len = 0;
	uint16_t cid, sqhd;
	uint32_t *ptr;

	/* Sanity check on return of outstanding command */
	if (!lpfc_cmd || !lpfc_cmd->nvmeCmd || !lpfc_cmd->nrport) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NODE | LOG_NVME,
				 "6071 Completion pointers bad on wqe %p.\n",
				 wcqe);
		return;
	}

	nCmd = lpfc_cmd->nvmeCmd;
	rport = lpfc_cmd->nrport;

	/*
	 * Catch race where our node has transitioned, but the
	 * transport is still transitioning.
	 */
	ndlp = rport->ndlp;
	if (!ndlp || !NLP_CHK_NODE_ACT(ndlp)) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NODE | LOG_NVME,
				 "6061 rport %p, ndlp %p, DID x%06x ndlp not ready.\n",
				 rport, ndlp, rport->remoteport->port_id);

		ndlp = lpfc_findnode_did(vport, rport->remoteport->port_id);
		if (!ndlp) {
			lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
					 "6062 Ignoring NVME cmpl.  No ndlp\n");
			goto out_err;
		}
	}

	code = bf_get(lpfc_wcqe_c_code, wcqe);
	if (code == CQE_CODE_NVME_ERSP) {
		/* For this type of CQE, we need to rebuild the rsp */

		/*
		 * Get Command Id from cmd to plug into response. This
		 * code is not needed in the next NVME Transport drop.
		 */
		ptr = (uint32_t *)nCmd->cmdaddr;/* to be removed */
		ptr += 8;			/* to be removed */
		code = *ptr;			/* to be removed */
		code = le32_to_cpu(code);	/* to be removed */
		cid = (code >> 16) & 0xffff;	/* to be removed */

		/*
		 * RSN is in CQE word 2
		 * SQHD is in CQE Word 3 bits 15:0
		 * NOTE: information in CQE is Little Endian
		 */
		ptr = (uint32_t *)wcqe;
		sqhd = (uint16_t)(*(ptr+3) & 0xffff);

		/* Build response */
		ptr = (uint32_t *)nCmd->rspaddr;
		*ptr++ = cpu_to_be32(8);  /* ERSP IU Length */
		*ptr++ = cpu_to_be32(wcqe->parameter); /* RSN */
		*ptr++ = 0;		 /* Word 2 - reserved */
		*ptr++ = 0;		 /* Word 3 - reserved */
		*ptr++ = 0;		 /* Word 4 */
		*ptr++ = 0;		 /* Word 5 */
		/* SQ ID is 0, SQHD from CQE */
		*ptr++ = cpu_to_be32(sqhd);

		/* Cmd ID from cmd payload */
		*ptr = cid;	/* to be removed */

		lpfc_cmd->status = IOSTAT_SUCCESS;
		lpfc_cmd->result = 0;
	} else {
		lpfc_cmd->status = (bf_get(lpfc_wcqe_c_status, wcqe) &
			    LPFC_IOCB_STATUS_MASK);
		lpfc_cmd->result = wcqe->parameter;
	}

	/* For NVME, the only failure path that results in an
	 * IO error is when the adapter rejects it.  All other
	 * conditions are a success case and resolved by the
	 * transport.
	 */
	if ((lpfc_cmd->status == IOSTAT_SUCCESS) ||
	    (lpfc_cmd->status == IOSTAT_FCP_RSP_ERROR)) {
		nCmd->transferred_length = wcqe->total_data_placed;
		nCmd->rcv_rsplen = 0;
		if (lpfc_cmd->status == IOSTAT_FCP_RSP_ERROR)
			nCmd->rcv_rsplen = wcqe->parameter;
		nCmd->status = 0;
	} else
		goto out_err;

	lpfc_printf_vlog(vport, KERN_WARNING, LOG_NVME | LOG_FCP,
			 "6059 NVME cmd %p completion "
			 "io status: x%x rcv_rsplen: x%x "
			 "sid: x%06x did: x%06x oxid: x%x "
			 "total data placed x%x\n",
			 nCmd, lpfc_cmd->status, nCmd->rcv_rsplen,
			 vport->fc_myDID,
			 (ndlp) ? ndlp->nlp_DID : 0,
			 lpfc_cmd->cur_iocbq.sli4_xritag,
			 nCmd->transferred_length);

	/* pick up SLI4 exhange busy condition */
	if (bf_get(lpfc_wcqe_c_xb, wcqe))
		lpfc_cmd->flags |= LPFC_SBUF_XBUSY;
	else
		lpfc_cmd->flags &= ~LPFC_SBUF_XBUSY;

	if (ndlp && NLP_CHK_NODE_ACT(ndlp))
		atomic_dec(&ndlp->cmd_pending);

	/* Update stats and complete the IO.  There is
	 * no need for dma unprep because the nvme_transport
	 * owns the dma address.
	 */
	nCmd->done(nCmd);

	spin_lock_irqsave(&phba->hbalock, flags);
	lpfc_cmd->nvmeCmd = NULL;
	lpfc_cmd->nrport = NULL;
	spin_unlock_irqrestore(&phba->hbalock, flags);

	goto out_cleanup;

 out_err:
	/* The lpfc_cmd is valid, but the ndlp may not be - don't
	 * touch it.
	 */
	lpfc_cmd->result = wcqe->parameter;
	nCmd->transferred_length = 0;
	nCmd->rcv_rsplen = nCmd->rsplen;
	nCmd->status = -EINVAL;
	len = wcqe->parameter;
	if (wcqe->parameter == 0)
		len = nCmd->rsplen;

	lpfc_printf_vlog(vport, KERN_WARNING, LOG_NVME | LOG_FCP,
			 "6072 NVME Completion Error: status x%x, result x%x "
			 "returning %d, rsplen %d.\n", lpfc_cmd->status,
			 lpfc_cmd->result, nCmd->status,
			 nCmd->rsplen);

 out_cleanup:
	lpfc_nvme_restore_fcp_sgls(vport, lpfc_cmd);
	lpfc_release_scsi_buf(phba, lpfc_cmd);
}


/**
 * lpfc_nvme_prep_io_cmd - Issue an NVME-over-FCP IO
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @lpfc_nvme_lport: Pointer to the driver's local port data
 * @lpfc_nvme_rport: Pointer to the rport getting the @lpfc_nvme_ereq
 * @lpfc_nvme_fcreq: IO request from nvme fc to driver.
 * @hw_queue_handle: Driver-returned handle in lpfc_nvme_create_hw_queue
 *
 * Driver registers this routine as it io request handler.  This
 * routine issues an fcp WQE with data from the @lpfc_nvme_fcpreq
 * data structure to the rport indicated in @lpfc_nvme_rport.
 *
 * Return value :
 *   0 - Success
 *   TODO: What are the failure codes.
 **/
static int
lpfc_nvme_prep_io_cmd(struct lpfc_vport *vport,
		      struct lpfc_scsi_buf *lpfc_cmd,
		      struct lpfc_nodelist *pnode)
{
	struct lpfc_hba *phba = vport->phba;
	struct nvmefc_fcp_req *nCmd = lpfc_cmd->nvmeCmd;
	struct fcp_cmnd *fcp_cmnd = lpfc_cmd->fcp_cmnd;
	union lpfc_wqe *wqe = &lpfc_cmd->cur_iocbq.wqe;
	struct lpfc_iocbq *pwqeq = &(lpfc_cmd->cur_iocbq);

	if (!pnode || !NLP_CHK_NODE_ACT(pnode))
		return -EINVAL;

	/*
	 * There are three possibilities here - use scatter-gather segment, use
	 * the single mapping, or neither.  Start the lpfc command prep by
	 * bumping the bpl beyond the fcp_cmnd and fcp_rsp regions to the first
	 * data bde entry.
	 */
	wqe->generic.wqe_com.word7 = 0;
	wqe->generic.wqe_com.word10 = 0;

	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME | LOG_MISC,
			 "6055 Prep NVME IO: sg_cnt %d, flags x%x\n",
			 nCmd->sg_cnt, nCmd->io_dir);
	if (nCmd->sg_cnt) {
		if (nCmd->io_dir == NVMEFC_FCP_WRITE) {
			/* Word 3 */
			/* Add the FCP_CMD and FCP_RSP sizes for the offset */
			bf_set(payload_offset_len, &wqe->fcp_iwrite,
			       sizeof(struct fcp_cmnd) +
			       sizeof(struct fcp_rsp));

			/* Word 7 */
			bf_set(wqe_cmnd, &wqe->generic.wqe_com,
			       CMD_FCP_IWRITE64_WQE);
			bf_set(wqe_pu, &wqe->generic.wqe_com,
			       PARM_READ_CHECK);

			/* Word 10 */
			bf_set(wqe_iod, &wqe->fcp_iwrite.wqe_com,
			       LPFC_WQE_IOD_WRITE);
			bf_set(wqe_lenloc, &wqe->fcp_iwrite.wqe_com,
			       LPFC_WQE_LENLOC_WORD4);
			bf_set(wqe_ebde_cnt, &wqe->fcp_iwrite.wqe_com, 0);
			bf_set(wqe_dbde, &wqe->fcp_iwrite.wqe_com, 1);

			/* Word 11 */
			bf_set(wqe_cmd_type, &wqe->generic.wqe_com,
			       NVME_WRITE_CMD);

			fcp_cmnd->fcpCntl3 = WRITE_DATA;
			phba->fc4OutputRequests++;
		} else {
			/* Read IO.  Set up Word 3. */
			/* Add the FCP_CMD and FCP_RSP sizes for the offset */
			bf_set(payload_offset_len, &wqe->fcp_iread,
			       sizeof(struct fcp_cmnd) +
			       sizeof(struct fcp_rsp));

			/* Word 7 */
			bf_set(wqe_cmnd, &wqe->generic.wqe_com,
			       CMD_FCP_IREAD64_WQE);
			bf_set(wqe_pu, &wqe->generic.wqe_com,
			       PARM_READ_CHECK);

			/* Word 10 */
			bf_set(wqe_iod, &wqe->fcp_iread.wqe_com,
			       LPFC_WQE_IOD_READ);
			bf_set(wqe_lenloc, &wqe->fcp_iread.wqe_com,
			       LPFC_WQE_LENLOC_WORD4);
			bf_set(wqe_ebde_cnt, &wqe->fcp_iread.wqe_com, 0);
			bf_set(wqe_dbde, &wqe->fcp_iread.wqe_com, 1);

			/* Word 11 */
			bf_set(wqe_cmd_type, &wqe->generic.wqe_com,
			       NVME_READ_CMD);

			fcp_cmnd->fcpCntl3 = READ_DATA;
			phba->fc4InputRequests++;
		}
	} else {
		/* Word 4 */
		wqe->fcp_icmd.rsrvd4 = 0;

		/* Word 7 */
		bf_set(wqe_cmnd, &wqe->generic.wqe_com, CMD_FCP_ICMND64_WQE);
		bf_set(wqe_pu, &wqe->generic.wqe_com, 0);

		/* Word 10 */
		bf_set(wqe_dbde, &wqe->fcp_icmd.wqe_com, 1);
		bf_set(wqe_iod, &wqe->fcp_icmd.wqe_com, LPFC_WQE_IOD_WRITE);
		bf_set(wqe_qosd, &wqe->fcp_icmd.wqe_com, 1);
		bf_set(wqe_lenloc, &wqe->fcp_icmd.wqe_com,
		       LPFC_WQE_LENLOC_NONE);
		bf_set(wqe_ebde_cnt, &wqe->fcp_icmd.wqe_com, 0);

		/* Word 11 */
		bf_set(wqe_cmd_type, &wqe->generic.wqe_com, NVME_READ_CMD);

		fcp_cmnd->fcpCntl3 = 0;
		phba->fc4ControlRequests++;
	}

	/*
	 * Finish initializing those WQE fields that are independent
	 * of the scsi_cmnd request_buffer
	 */

	/* Word 6 */
	bf_set(wqe_ctxt_tag, &wqe->generic.wqe_com,
	       phba->sli4_hba.rpi_ids[pnode->nlp_rpi]);
	bf_set(wqe_xri_tag, &wqe->generic.wqe_com, pwqeq->sli4_xritag);

	/* Word 7:  Set erp to 0 for NVME.  */
	bf_set(wqe_erp, &wqe->generic.wqe_com, 0);

	/* Preserve Class data in the ndlp. */
	bf_set(wqe_class, &wqe->generic.wqe_com,
	       (pnode->nlp_fcp_info & 0x0f));

	/* NVME upper layers will time things out, if needed */
	bf_set(wqe_tmo, &wqe->generic.wqe_com, 0);

	/* Word 8 */
	wqe->generic.wqe_com.abort_tag = pwqeq->iotag;

	/* Word 9 */
	bf_set(wqe_reqtag, &wqe->generic.wqe_com, pwqeq->iotag);

	/* Word 11 */
	bf_set(wqe_cqid, &wqe->generic.wqe_com, LPFC_WQE_CQ_ID_DEFAULT);

	pwqeq->context1 = lpfc_cmd;
	if (pwqeq->wqe_cmpl == NULL)
		pwqeq->wqe_cmpl = lpfc_nvme_io_cmd_wqe_cmpl;
	pwqeq->iocb_cmpl = NULL;
	pwqeq->vport = vport;
	pwqeq->iocb_flag |= LPFC_IO_NVME;
	return 0;
}


/**
 * lpfc_nvme_prep_io_dma - Issue an NVME-over-FCP IO
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @lpfc_nvme_lport: Pointer to the driver's local port data
 * @lpfc_nvme_rport: Pointer to the rport getting the @lpfc_nvme_ereq
 * @lpfc_nvme_fcreq: IO request from nvme fc to driver.
 * @hw_queue_handle: Driver-returned handle in lpfc_nvme_create_hw_queue
 *
 * Driver registers this routine as it io request handler.  This
 * routine issues an fcp WQE with data from the @lpfc_nvme_fcpreq
 * data structure to the rport indicated in @lpfc_nvme_rport.
 *
 * Return value :
 *   0 - Success
 *   TODO: What are the failure codes.
 **/
static int
lpfc_nvme_prep_io_dma(struct lpfc_vport *vport,
		      struct lpfc_scsi_buf *lpfc_cmd)
{
	struct lpfc_hba *phba = vport->phba;
	struct nvmefc_fcp_req *nCmd = lpfc_cmd->nvmeCmd;
	struct fcp_cmnd *fcp_cmnd = lpfc_cmd->fcp_cmnd;
	union lpfc_wqe *wqe_cmd = &lpfc_cmd->cur_iocbq.wqe;
	struct sli4_sge *sgl = (struct sli4_sge *)lpfc_cmd->fcp_bpl;
	struct scatterlist *data_sg;
	struct sli4_sge *first_data_sgl;
	dma_addr_t physaddr;
	uint32_t num_bde = 0;
	uint32_t dma_len;
	uint32_t dma_offset = 0;
	int nseg, i;

	/* Fix up the command and response DMA stuff. */
	lpfc_nvme_adj_fcp_sgls(vport, lpfc_cmd, nCmd);

	/*
	 * There are three possibilities here - use scatter-gather segment, use
	 * the single mapping, or neither.  Start the lpfc command prep by
	 * bumping the bpl beyond the fcp_cmnd and fcp_rsp regions to the first
	 * data bde entry.
	 */
	if (nCmd->sg_cnt) {
		/*
		 * Jump over the fcp_cmd and fcp_rsp.  The fix routine
		 * has already adjusted for this.
		 */
		sgl += 2;

		first_data_sgl = sgl;
		lpfc_cmd->seg_cnt = nCmd->sg_cnt;
		if (lpfc_cmd->seg_cnt > phba->cfg_sg_seg_cnt) {
			lpfc_printf_log(phba, KERN_ERR, LOG_NVME,
					"6058 Too many sg segments from "
					"NVME Transport.  Max %d, "
					"nvmeIO sg_cnt %d\n",
					phba->cfg_sg_seg_cnt,
					lpfc_cmd->seg_cnt);
			lpfc_cmd->seg_cnt = 0;
			return 1;
		}

		/*
		 * The driver established a maximum scatter-gather segment count
		 * during probe that limits the number of sg elements in any
		 * single scsi command.  Just run through the seg_cnt and format
		 * the sge's.
		 */
		nseg = nCmd->sg_cnt;
		data_sg = nCmd->first_sgl;
		for (i = 0; i < nseg; i++) {
			if (data_sg == NULL) {
				lpfc_printf_log(phba, KERN_ERR, LOG_NVME,
					"9999 Segment count mismatch: %d "
					"nvmeIO sg_cnt: %d\n", i, nseg);
				lpfc_cmd->seg_cnt = 0;
				return 1;
			}
			physaddr = data_sg->dma_address;
			dma_len = data_sg->length;
			sgl->addr_lo = cpu_to_le32(putPaddrLow(physaddr));
			sgl->addr_hi = cpu_to_le32(putPaddrHigh(physaddr));
			sgl->word2 = le32_to_cpu(sgl->word2);
			if ((num_bde + 1) == nseg)
				bf_set(lpfc_sli4_sge_last, sgl, 1);
			else
				bf_set(lpfc_sli4_sge_last, sgl, 0);
			bf_set(lpfc_sli4_sge_offset, sgl, dma_offset);
			bf_set(lpfc_sli4_sge_type, sgl, LPFC_SGE_TYPE_DATA);
			sgl->word2 = cpu_to_le32(sgl->word2);
			sgl->sge_len = cpu_to_le32(dma_len);

			lpfc_printf_log(phba, KERN_INFO, LOG_NVME | LOG_FCP,
					"9999 Set DMA seg: addr x%llx, "
					"len x%x, seg %d of %d\n",
					physaddr, dma_len, i, nseg);
			dma_offset += dma_len;
			data_sg = sg_next(data_sg);
			sgl++;
		}
	} else {
		/* For this clause to be valid, the payload_length
		 * and sg_cnt must zero.
		 */
		if (nCmd->payload_length != 0) {
			lpfc_printf_log(phba, KERN_ERR, LOG_NVME | LOG_FCP,
					"9999 NVME DMA Prep Err: sg_cnt %d "
					"payload_length x%x\n",
					nCmd->sg_cnt, nCmd->payload_length);
			return 1;
		}
	}

	/*
	 * Finish initializing those WQE fields that are dependent on the
	 * scsi_cmnd request_buffer.
	 */
	fcp_cmnd->fcpDl = cpu_to_be32(nCmd->payload_length);

	/*
	 * Due to difference in data length between DIF/non-DIF paths,
	 * we need to set word 4 of WQE here
	 */
	wqe_cmd->fcp_iread.total_xfer_len = nCmd->payload_length;
	return 0;
}

/**
 * lpfc_nvme_fcp_io_submit - Issue an NVME-over-FCP IO
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @lpfc_nvme_lport: Pointer to the driver's local port data
 * @lpfc_nvme_rport: Pointer to the rport getting the @lpfc_nvme_ereq
 * @lpfc_nvme_fcreq: IO request from nvme fc to driver.
 * @hw_queue_handle: Driver-returned handle in lpfc_nvme_create_hw_queue
 *
 * Driver registers this routine as it io request handler.  This
 * routine issues an fcp WQE with data from the @lpfc_nvme_fcpreq
 * data structure to the rport
 indicated in @lpfc_nvme_rport.
 *
 * Return value :
 *   0 - Success
 *   TODO: What are the failure codes.
 **/
static int
lpfc_nvme_fcp_io_submit(struct nvme_fc_local_port *pnvme_lport,
			struct nvme_fc_remote_port *pnvme_rport,
			void *hw_queue_handle,
			struct nvmefc_fcp_req *pnvme_fcreq)
{
	int ret = 0;
	struct lpfc_nvme_lport *lport;
	struct lpfc_vport *vport;
	struct lpfc_hba *phba;
	struct lpfc_nodelist *ndlp;
	struct lpfc_scsi_buf *lpfc_cmd;
	struct lpfc_nvme_rport *rport;

	lport = (struct lpfc_nvme_lport *)pnvme_lport->private;
	rport = (struct lpfc_nvme_rport *)pnvme_rport->private;
	vport = lport->pnvme->vport;
	phba = vport->phba;

	/* Announce entry to new IO submit field. */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME,
			 "6002 ENTER.  Issue IO to rport %p, DID x%06x "
			 "on lport %p Data: %p %p\n",
			 pnvme_rport, pnvme_rport->port_id, pnvme_lport,
			 pnvme_fcreq, hw_queue_handle);

	/*
	 * Catch race where our node has transitioned, but the
	 * transport is still transitioning.
	 */
	ndlp = rport->ndlp;
	if (!ndlp || !NLP_CHK_NODE_ACT(ndlp)) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NODE | LOG_NVME,
				 "6053 rport %p, ndlp %p, DID x%06x ndlp not ready.\n",
				 rport, ndlp, pnvme_rport->port_id);

		ndlp = lpfc_findnode_did(vport, pnvme_rport->port_id);
		if (!ndlp) {
			lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
					 "9999 Could not find node for DID %x\n",
					 pnvme_rport->port_id);
			ret = -ENODEV;
			goto out_fail;
		}
	}

	/* The remote node has to be ready for IO or it's an error. */
	if ((ndlp->nlp_state != NLP_STE_MAPPED_NODE) &&
	    !(ndlp->nlp_type & NLP_NVME_TARGET)) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NODE | LOG_NVME,
				 "6036 rport %p, DID x%06x not ready for "
				 "IO. State x%x, Type x%x\n",
				 rport, pnvme_rport->port_id,
				 ndlp->nlp_state, ndlp->nlp_type);
		ret = -ENODEV;
		goto out_fail;

	}

	/* The node is shared with FCP IO, make sure the IO pending count does
	 * not exceed the programmed depth.
	 */
	if (atomic_read(&ndlp->cmd_pending) >= ndlp->cmd_qdepth) {
		ret = -EAGAIN;
		goto out_fail;
	}

	/* For the prototype, the driver is reusing the lpfc_scsi_buf. */
	lpfc_cmd = lpfc_get_scsi_buf(phba, ndlp);
	if (lpfc_cmd == NULL) {
		lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME | LOG_MISC,
				 "6065 driver's buffer pool is empty, "
				 "IO failed\n");
		ret = -ENOMEM;
		goto out_fail;
	}

	/*
	 * Store the data needed by the driver to issue and complete the IO.
	 * Do not let the IO hang out forever.  There is no midlayer issuing
	 * an abort so inform the FW of the maximum IO pending time.
	 */
	lpfc_cmd->nvmeCmd = pnvme_fcreq;
	lpfc_cmd->nrport = rport;

	lpfc_cmd->start_time = jiffies;
	lpfc_cmd->cur_iocbq.wqe_cmpl = NULL;

	lpfc_nvme_prep_io_cmd(vport, lpfc_cmd, ndlp);
	ret = lpfc_nvme_prep_io_dma(vport, lpfc_cmd);
	if (ret) {
		ret = -ENOMEM;
		goto out_free_scsi_buf;
	}

	atomic_inc(&ndlp->cmd_pending);
	lpfc_printf_vlog(vport, KERN_INFO, LOG_FCP | LOG_NVME,
			 "9999 Issuing NVME IO to rport %p, "
			 "DID x%06x on lport %p Data: %p x%llx\n",
			 pnvme_rport, pnvme_rport->port_id, pnvme_lport,
			 pnvme_fcreq, (uint64_t) hw_queue_handle);

	ret = lpfc_sli_issue_wqe(phba, LPFC_FCP_RING, &lpfc_cmd->cur_iocbq);
	if (ret) {
		atomic_dec(&ndlp->cmd_pending);
		lpfc_printf_vlog(vport, KERN_INFO, LOG_FCP | LOG_NVME,
				 "6056 FCP could not issue WQE err %x "
				 "sid: x%x did: x%x oxid: x%x\n",
				 ret, vport->fc_myDID, ndlp->nlp_DID,
				 lpfc_cmd->cur_iocbq.sli4_xritag);
		ret = -EINVAL;
		goto out_free_scsi_buf;
	}
	return 0;

 out_free_scsi_buf:
	lpfc_nvme_restore_fcp_sgls(vport, lpfc_cmd);
	lpfc_release_scsi_buf(phba, lpfc_cmd);
 out_fail:
	return ret;
}

/**
 * lpfc_nvme_fcp_abort - Issue an NVME-over-FCP ABTS
 * @lpfc_pnvme: Pointer to the driver's nvme instance data
 * @lpfc_nvme_lport: Pointer to the driver's local port data
 * @lpfc_nvme_rport: Pointer to the rport getting the @lpfc_nvme_ereq
 * @lpfc_nvme_fcreq: IO request from nvme fc to driver.
 * @hw_queue_handle: Driver-returned handle in lpfc_nvme_create_hw_queue
 *
 * Driver registers this routine as it io abort handler.  This
 * routine issues an fcp WQE with data from the @lpfc_nvme_fcpreq
 * data structure to the rport
 indicated in @lpfc_nvme_rport.
 *
 * Return value :
 *   0 - Success
 *   TODO: What are the failure codes.
 **/
static void
lpfc_nvme_fcp_abort(struct nvme_fc_local_port *pnvme_lport,
			struct nvme_fc_remote_port *pnvme_rport,
			void *hw_queue_handle,
			struct nvmefc_fcp_req *pnvme_fcreq)
{
	struct lpfc_nvme_lport *lport;
	struct lpfc_vport *vport;
	struct lpfc_hba *phba;
	struct lpfc_nodelist *ndlp;
	struct lpfc_nvme_rport *rport;

	lport = (struct lpfc_nvme_lport *)pnvme_lport->private;
	rport = (struct lpfc_nvme_rport *)pnvme_rport->private;
	vport = lport->pnvme->vport;
	phba = vport->phba;

	/* Announce entry to new IO submit field. */
	lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME,
			 "6002 ENTER.  Issue IO to rport %p, DID x%06x "
			 "on lport %p Data: %p x%llx\n",
			 pnvme_rport, pnvme_rport->port_id, pnvme_lport,
			 pnvme_fcreq, (uint64_t) hw_queue_handle);

	/*
	 * Catch race where our node has transitioned, but the
	 * transport is still transitioning.
	 */
	ndlp = rport->ndlp;
	if (!ndlp || !NLP_CHK_NODE_ACT(ndlp)) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NODE | LOG_NVME,
				 "6053 rport %p, ndlp %p, DID x%06x ndlp not ready.\n",
				 rport, ndlp, pnvme_rport->port_id);

		ndlp = lpfc_findnode_did(vport, pnvme_rport->port_id);
		if (!ndlp) {
			lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
					 "9999 Could not find node for DID %x\n",
					 pnvme_rport->port_id);
			goto out_fail;
		}
	}

	/* The remote node has to be ready for IO or it's an error. */
	if ((ndlp->nlp_state != NLP_STE_MAPPED_NODE) &&
	    !(ndlp->nlp_type & NLP_NVME_TARGET)) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NODE | LOG_NVME,
				 "6036 rport %p, DID x%06x not ready for "
				 "IO. State x%x, Type x%x\n",
				 rport, pnvme_rport->port_id,
				 ndlp->nlp_state, ndlp->nlp_type);
		goto out_fail;

	}

 out_fail:
	return;
}

/* Declare and initialization an instance of the FC NVME template. */
static struct nvme_fc_port_template lpfc_nvme_template = {
	/* initiator-based functions */
	.create_queue = lpfc_nvme_create_hw_queue,
	.delete_queue = lpfc_nvme_delete_hw_queue,
	.ls_req       = lpfc_nvme_ls_req,
	.fcp_io       = lpfc_nvme_fcp_io_submit,
	.ls_abort     = lpfc_nvme_ls_abort,
	.fcp_abort    = lpfc_nvme_fcp_abort,

	/* TBD.  Set max_hw_queues for now.  */
	.max_hw_queues = 1,	/* LPFC_HBA_IO_CHAN_MAX, */
	.max_sgl_segments = 16,
	.max_dif_sgl_segments = 16,
	.dma_boundary = 0xFFFFFFFF,

	/* Sizes of additional private data for data structures.
	 * No use for the last two sizes at this time.
	 */
	.local_priv_sz = sizeof(struct lpfc_nvme_lport),
	.remote_priv_sz = sizeof(struct lpfc_nvme_rport),
	.lsrqst_priv_sz = 0,
	.fcprqst_priv_sz = 0,
};

/**
 * lpfc_create_nvme_lport - Create/Bind an nvme localport instance.
 * @pvport - the lpfc_vport instance requesting a localport.
 *
 * This routine is invoked to create an nvme localport instance to bind
 * to the nvme_fc_transport.  It is called once during driver load
 * like lpfc_create_shost after all other services are initialized.
 * It requires a vport, vpi, and wwns at call time.  Other localport
 * parameters are modified as the driver's FCID and the Fabric WWN
 * are established.
 *
 * Return codes
 *      0 - successful
 *      -ENOMEM - no heap memory available
 *      other values - from nvme registration upcall
 **/
int
lpfc_create_nvme_lport(struct lpfc_vport *vport)
{
	struct nvme_fc_port_info nfcp_info;
	struct nvme_fc_local_port *localport;
	struct lpfc_nvme_lport *lport;
	struct lpfc_nvme *pnvme = vport->pnvme;
	int len, ret = 0;

	/* Allocate memory for the NVME instance. */
	pnvme = kzalloc(sizeof(struct lpfc_nvme), GFP_KERNEL);
	if (!pnvme) {
		lpfc_printf_vlog(vport, KERN_ERR, LOG_NVME,
				 "6003 Failed to allocate nvme struct\n");
		return -ENOMEM;
	}

	/* Complete initializing the nvme instance including back pointers. */
	vport->pnvme = pnvme;
	pnvme->vport = vport;
	pnvme->lpfc_nvme_state = LPFC_NVME_INIT;
	pnvme->lpfc_nvme_conn_state = LPFC_NVME_CONN_NONE;
	INIT_LIST_HEAD(&pnvme->lport_list);

	/* Initialize this localport instance.  The vport wwn usage ensures
	 * that NPIV is accounted for.
	 */
	memset(&nfcp_info, 0, sizeof(struct nvme_fc_port_info));
	nfcp_info.port_role = FC_PORT_ROLE_NVME_INITIATOR;
	nfcp_info.node_name = wwn_to_u64(vport->fc_nodename.u.wwn);
	nfcp_info.port_name = wwn_to_u64(vport->fc_portname.u.wwn);

	/* localport is allocated from the stack, but the registration
	 * call allocates heap memory as well as the private area.
	 */
	ret = nvme_fc_register_localport(&nfcp_info, &lpfc_nvme_template,
					 &vport->phba->pcidev->dev, &localport);
	if (!ret) {
		lpfc_printf_vlog(vport, KERN_INFO, LOG_NVME,
				 "6005 Successfully registered local "
				 "NVME port num %d, localP %p, lport priv %p\n",
				 localport->port_num, localport,
				 localport->private);

		/* Private is our lport size declared in the template. */
		lport = (struct lpfc_nvme_lport *) localport->private;
		lport->localport = localport;
		lport->pnvme = pnvme;
		INIT_LIST_HEAD(&lport->list);
		INIT_LIST_HEAD(&lport->rport_list);
		list_add_tail(&lport->list, &pnvme->lport_list);
	}
	len  = lpfc_new_scsi_buf(vport, 32);
	vport->phba->total_scsi_bufs += len;
	return ret;
}

/**
 * lpfc_destroy_nvme_lport - Destroy lpfc_nvme bound to nvme transport.
 * @pnvme: pointer to lpfc nvme data structure.
 *
 * This routine is invoked to destroy all lports bound to the phba.
 * The lport memory was allocated by the nvme fc transport and is
 * released there.  This routine ensures all rports bound to the
 * lport have been disconnected.
 *
 **/
void
lpfc_destroy_nvme_lport(struct lpfc_nvme *pnvme)
{
	struct lpfc_nvme_lport *lport, *lport_next;
	int ret;

	lpfc_printf_vlog(pnvme->vport, KERN_INFO, LOG_NVME,
			 "6007 Destroying NVME lport %p\n",
			 pnvme);

	list_for_each_entry_safe(lport, lport_next, &pnvme->lport_list, list) {
		if (!list_empty(&lport->rport_list)) {
			lpfc_printf_vlog(pnvme->vport, KERN_ERR, LOG_NVME,
					 "6008 lport %p rport list not empty.  "
					 "Fail destroy.\n",
					 lport);
			return;
		}
		/*
		 * lport's rport list is clear.  Unregister lport and
		 * release resources.
		 */
		list_del(&lport->list);
		ret = nvme_fc_unregister_localport(lport->localport);
		if (ret == 0)
			lpfc_printf_vlog(pnvme->vport,
					 KERN_INFO, LOG_NVME,
					 "6009 Unregistered lport "
					 "Success\n");
		else
			lpfc_printf_vlog(pnvme->vport,
					 KERN_INFO, LOG_NVME,
					 "6010 Unregistered lport "
					 "Failed, status x%x\n",
					 ret);
	}

	/* All lports are unregistered.  Safe to free nvme memory. */
	kfree(pnvme);
}

