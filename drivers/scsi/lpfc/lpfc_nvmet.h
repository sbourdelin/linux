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

#define LPFC_MAX_NQN_SZ	256

/* Used for NVME Target */
struct lpfc_nvmet_tgtport {
	struct lpfc_hba *phba;
	enum nvme_state nvmet_state;

	/* Stats counters */
	uint32_t rcv_ls_req;
	uint32_t rcv_ls_drop;
	uint32_t xmt_ls_rsp;
	uint32_t xmt_ls_drop;
	uint32_t xmt_ls_rsp_error;
	uint32_t xmt_ls_rsp_cmpl;

	uint32_t rcv_fcp_cmd;
	uint32_t rcv_fcp_drop;
	uint32_t xmt_fcp_rsp_cmpl;
	uint32_t xmt_fcp_rsp;
	uint32_t xmt_fcp_drop;
	uint32_t xmt_fcp_rsp_error;

	uint32_t xmt_abort_rsp;
	uint32_t xmt_abort_cmpl;
	uint32_t xmt_abort_rsp_error;
};

struct lpfc_nvmet_rcv_ctx {
	union {
		struct nvmefc_tgt_ls_req ls_req;
		struct nvmefc_tgt_fcp_req fcp_req;
	} ctx;
	struct lpfc_hba *phba;
	struct lpfc_iocbq *wqeq;
	dma_addr_t txrdy_phys;
	uint32_t *txrdy;
	uint32_t sid;
	uint32_t offset;
	uint16_t oxid;
	uint16_t size;
	uint16_t entry_cnt;
	uint16_t state;
	struct hbq_dmabuf *hbq_buffer;
/* States */
#define LPFC_NVMET_STE_RCV		1
#define LPFC_NVMET_STE_DATA		2
#define LPFC_NVMET_STE_ABORT		3
#define LPFC_NVMET_STE_RSP		4
#define LPFC_NVMET_STE_DONE		5
};

