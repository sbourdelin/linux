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

enum nvme_conn_state {
	LPFC_NVME_CONN_ERR     =  0,    /* Connections have error */
	LPFC_NVME_CONN_NONE    =  1,    /* No connections available */
	LPFC_NVME_IN_PROGRESS  =  6,    /* Connections in progress */
	LPFC_NVME_CONN_RDY     =  7     /* Connections ready for IO */
};

enum nvme_state {
	LPFC_NVME_INIT    =   0,   /* NVME Struct alloc and initialize */
	LPFC_NVME_REG     =   1,   /* NVME driver inst reg'd with OS */
	LPFC_NVME_READY   =   2,   /* NVME instance ready for connections */
	LPFC_NVME_ERROR   =   3    /* NVME instance in error */
};

struct lpfc_nvme_qhandle {
	uint32_t cpu_id;
	uint32_t wq_id;
};

struct lpfc_nvme {
	struct lpfc_vport *vport;
	enum nvme_conn_state lpfc_nvme_conn_state;
	enum nvme_state lpfc_nvme_state;
	struct list_head lport_list;
};

/* Declare nvme-based local and remote port definitions. */
struct lpfc_nvme_lport {
	struct list_head list;
	struct lpfc_nvme *pnvme;
	struct nvme_fc_local_port *localport;
	struct list_head rport_list;
};

struct lpfc_nvme_rport {
	struct list_head list;
	struct lpfc_nvme_lport *lport;
	struct nvme_fc_remote_port *remoteport;
	struct lpfc_nodelist *ndlp;
};

