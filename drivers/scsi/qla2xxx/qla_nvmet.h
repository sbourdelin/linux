/*
 * QLogic Fibre Channel HBA Driver
 * Copyright (c)  2003-2017 QLogic Corporation
 *
 * See LICENSE.qla2xxx for copyright and licensing details.
 */
#ifndef __QLA_NVMET_H
#define __QLA_NVMET_H

#include <linux/blk-mq.h>
#include <uapi/scsi/fc/fc_fs.h>
#include <uapi/scsi/fc/fc_els.h>
#include <linux/nvme-fc-driver.h>

#include "qla_def.h"

struct qla_nvmet_tgtport {
	struct scsi_qla_host *vha;
	struct completion tport_del;
};

struct qla_nvmet_cmd {
	union {
		struct nvmefc_tgt_ls_req ls_req;
		struct nvmefc_tgt_fcp_req fcp_req;
	} cmd;
	struct scsi_qla_host *vha;
	void *buf;
	struct atio_from_isp atio;
	struct atio7_nvme_cmnd nvme_cmd_iu;
	uint16_t cmd_len;
	spinlock_t nvme_cmd_lock;
	struct list_head cmd_list; /* List of cmds */
	struct work_struct work;

	struct scatterlist *sg;	/* cmd data buffer SG vector */
	int sg_cnt;		/* SG segments count */
	int bufflen;		/* cmd buffer length */
	int offset;
	enum dma_data_direction dma_data_direction;
	uint16_t ox_id;
	struct fc_port *fcport;
};

#define CTIO_NVME    0x82            /* CTIO FC-NVMe IOCB */
struct ctio_nvme_to_27xx {
	uint8_t entry_type;             /* Entry type. */
	uint8_t entry_count;            /* Entry count. */
	uint8_t sys_define;             /* System defined. */
	uint8_t entry_status;           /* Entry Status. */

	uint32_t handle;                /* System handle. */
	uint16_t nport_handle;          /* N_PORT handle. */
	uint16_t timeout;               /* Command timeout. */

	uint16_t dseg_count;            /* Data segment count. */
	uint8_t	 vp_index;		/* vp_index */
	uint8_t  addl_flags;		/* Additional flags */

	uint8_t  initiator_id[3];	/* Initiator ID */
	uint8_t	 rsvd1;

	uint32_t exchange_addr;		/* Exch addr */

	uint16_t ox_id;			/* Ox ID */
	uint16_t flags;
#define NVMET_CTIO_STS_MODE0 0
#define NVMET_CTIO_STS_MODE1 BIT_6
#define NVMET_CTIO_STS_MODE2 BIT_7
#define NVMET_CTIO_SEND_STATUS BIT_15
	union {
		struct {
			uint8_t reserved1[8];
			uint32_t relative_offset;
			uint8_t	reserved2[4];
			uint32_t transfer_len;
			uint8_t reserved3[4];
			uint32_t dsd0[2];
			uint32_t dsd0_len;
		} nvme_status_mode0;
		struct {
			uint8_t nvme_comp_q_entry[16];
			uint32_t transfer_len;
			uint32_t rsp_seq_num;
			uint32_t dsd0[2];
			uint32_t dsd0_len;
		} nvme_status_mode1;
		struct {
			uint32_t reserved4[4];
			uint32_t transfer_len;
			uint32_t reserved5;
			uint32_t rsp_dsd[2];
			uint32_t rsp_dsd_len;
		} nvme_status_mode2;
	} u;
} __packed;

/*
 * ISP queue - CTIO type FC NVMe from ISP to target driver
 * returned entry structure.
 */
struct ctio_nvme_from_27xx {
	uint8_t	 entry_type;		    /* Entry type. */
	uint8_t	 entry_count;		    /* Entry count. */
	uint8_t	 sys_define;		    /* System defined. */
	uint8_t	 entry_status;		    /* Entry Status. */
	uint32_t handle;		    /* System defined handle */
	uint16_t status;
	uint16_t timeout;
	uint16_t dseg_count;		    /* Data segment count. */
	uint8_t  vp_index;
	uint8_t  reserved1[5];
	uint32_t exchange_address;
	uint16_t ox_id;
	uint16_t flags;
	uint32_t residual;
	uint8_t  reserved2[32];
} __packed;

int qla_nvmet_handle_ls(struct scsi_qla_host *vha,
	struct pt_ls4_rx_unsol *ls4, void *buf);
int qla_nvmet_create_targetport(struct scsi_qla_host *vha);
int qla_nvmet_delete(struct scsi_qla_host *vha);
int qla_nvmet_handle_abts(struct scsi_qla_host *vha,
	struct abts_recv_from_24xx *abts);
int qla_nvmet_process_cmd(struct scsi_qla_host *vha,
	struct qla_nvmet_cmd *cmd);

#endif
