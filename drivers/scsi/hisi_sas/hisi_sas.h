/*
 * Copyright (c) 2015 Linaro Ltd.
 * Copyright (c) 2015 Hisilicon Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#ifndef _HISI_SAS_H_
#define _HISI_SAS_H_

#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <scsi/libsas.h>

#define DRV_NAME "hisi_sas"
#define DRV_VERSION "v1.0"

#define HISI_SAS_MAX_PHYS	9
#define HISI_SAS_MAX_QUEUES	32
#define HISI_SAS_QUEUE_SLOTS 512
#define HISI_SAS_MAX_ITCT_ENTRIES 4096
#define HISI_SAS_MAX_DEVICES HISI_SAS_MAX_ITCT_ENTRIES
#define HISI_SAS_COMMAND_ENTRIES 8192

#define HISI_SAS_STATUS_BUF_SZ \
		(sizeof(struct hisi_sas_err_record) + 1024)
#define HISI_SAS_COMMAND_TABLE_SZ \
		(((sizeof(union hisi_sas_command_table)+3)/4)*4)

#define HISI_SAS_MAX_SSP_RESP_SZ (sizeof(struct ssp_frame_hdr) + 1024)
#define HISI_SAS_MAX_SMP_RESP_SZ 1028

#define HISI_SAS_NAME_LEN 32
#define HISI_SAS_RESET_REG_CNT 5

enum {
	PORT_TYPE_SAS = (1U << 1),
	PORT_TYPE_SATA = (1U << 0),
};

enum dev_status {
	HISI_SAS_DEV_NORMAL,
	HISI_SAS_DEV_EH,
};

enum hisi_sas_dev_type {
	HISI_SAS_DEV_TYPE_STP = 0,
	HISI_SAS_DEV_TYPE_SSP,
	HISI_SAS_DEV_TYPE_SATA,
};

struct hisi_sas_phy {
	struct hisi_hba	*hisi_hba;
	struct hisi_sas_port	*port;
	struct asd_sas_phy	sas_phy;
	struct sas_identify	identify;
	struct timer_list	timer;
	u64		port_id; /* from hw */
	u64		dev_sas_addr;
	u64		phy_type;
	u64		frame_rcvd_size;
	u8		frame_rcvd[32];
	u8		phy_attached;
	u8		reserved[3];
	u64		phy_event;
	int		eye_diag_done;
	enum sas_linkrate	minimum_linkrate;
	enum sas_linkrate	maximum_linkrate;
};

struct hisi_sas_port {
	struct asd_sas_port	sas_port;
	u8	port_attached;
	u8	id; /* from hw */
	struct list_head	list;
};

struct hisi_sas_cq {
	struct hisi_hba *hisi_hba;
	int	id;
};

struct hisi_sas_device {
	enum sas_device_type	dev_type;
	struct hisi_hba		*hisi_hba;
	struct domain_device	*sas_device;
	u64 attached_phy;
	u64 device_id;
	u64 running_req;
	struct hisi_sas_itct *itct;
	u8 dev_status;
	u64 reserved;
};

struct hisi_sas_slot {
	struct list_head entry;
	struct sas_task *task;
	struct hisi_sas_port	*port;
	u64	n_elem;
	int	dlvry_queue;
	int	dlvry_queue_slot;
	int	cmplt_queue;
	int	cmplt_queue_slot;
	int	idx;
	void	*cmd_hdr;
	dma_addr_t cmd_hdr_dma;
	void	*status_buffer;
	dma_addr_t status_buffer_dma;
	void *command_table;
	dma_addr_t command_table_dma;
	struct hisi_sas_sge_page *sge_page;
	dma_addr_t sge_page_dma;
};

struct hisi_sas_tmf_task {
	u8 tmf;
	u16 tag_of_task_to_be_managed;
};

struct hisi_sas_tei {
	struct sas_task	*task;
	struct hisi_sas_cmd_hdr	*hdr;
	struct hisi_sas_port	*port;
	struct hisi_sas_slot	*slot;
	int	n_elem;
	int	iptt;
};

enum hisi_sas_wq_event {
	PHYUP,
};

struct hisi_sas_wq {
	struct work_struct	work_struct;
	struct hisi_hba *hisi_hba;
	int phy_no;
	int event;
	int data;
};

struct hisi_hba {
	spinlock_t	lock;

	struct platform_device *pdev;

	void __iomem *regs;
	void __iomem *ctrl_regs;
	u32 reset_reg[HISI_SAS_RESET_REG_CNT];

	u8 sas_addr[SAS_ADDR_SIZE];

	struct hisi_sas_cmd_hdr	*cmd_hdr[HISI_SAS_MAX_QUEUES];
	dma_addr_t	cmd_hdr_dma[HISI_SAS_MAX_QUEUES];
	struct hisi_sas_complete_hdr	*complete_hdr[HISI_SAS_MAX_QUEUES];
	dma_addr_t	complete_hdr_dma[HISI_SAS_MAX_QUEUES];

	struct hisi_sas_initial_fis *initial_fis;
	dma_addr_t	initial_fis_dma;

	int	n_phy;

	int scan_finished;

	struct timer_list timer;
	struct workqueue_struct *wq;

	int slot_index_count;
	unsigned long *slot_index_tags;

	struct dma_pool *sge_page_pool;

	/* SCSI/SAS glue */
	struct sas_ha_struct sha;
	struct Scsi_Host *shost;

	struct hisi_sas_cq cq[HISI_SAS_MAX_QUEUES];
	struct hisi_sas_phy phy[HISI_SAS_MAX_PHYS];
	struct hisi_sas_port port[HISI_SAS_MAX_PHYS];

	int	id;
	int	queue_count;
	char	*int_names;
	struct hisi_sas_slot	*slot_prep;

	struct hisi_sas_device	devices[HISI_SAS_MAX_DEVICES];
	struct dma_pool *command_table_pool;
	struct dma_pool *status_buffer_pool;
	struct hisi_sas_itct *itct;
	dma_addr_t itct_dma;
	struct hisi_sas_iost *iost;
	dma_addr_t iost_dma;
	struct hisi_sas_breakpoint *breakpoint;
	dma_addr_t breakpoint_dma;
	struct hisi_sas_breakpoint *sata_breakpoint;
	dma_addr_t sata_breakpoint_dma;
	struct hisi_sas_slot	*slot_info;
};

/* Generic HW DMA host memory structures */
/* Delivery queue header */
struct hisi_sas_cmd_hdr {
	/* dw0 */
	__le32 dw0;

	/* dw1 */
	__le32 dw1;

	/* dw2 */
	__le32 dw2;

	/* dw3 */
	__le32 transfer_tags;

	/* dw4 */
	__le32 data_transfer_len;

	/* dw5 */
	__le32 first_burst_num;

	/* dw6 */
	__le32 sg_len;

	/* dw7 */
	__le32 dw7;

	/* dw8 */
	__le32 cmd_table_addr_lo;

	/* dw9 */
	__le32 cmd_table_addr_hi;

	/* dw10 */
	__le32 sts_buffer_addr_lo;

	/* dw11 */
	__le32 sts_buffer_addr_hi;

	/* dw12 */
	__le32 prd_table_addr_lo;

	/* dw13 */
	__le32 prd_table_addr_hi;

	/* dw14 */
	__le32 dif_prd_table_addr_lo;

	/* dw15 */
	__le32 dif_prd_table_addr_hi;
};

/* Completion queue header */
struct hisi_sas_complete_hdr {
	__le32 data;
};

struct hisi_sas_itct {
	__le64 qw0;
	__le64 sas_addr;
	__le64 qw2;
	__le64 qw3;
	__le64 qw4;
	__le64 qw_sata_ncq0_3;
	__le64 qw_sata_ncq7_4;
	__le64 qw_sata_ncq11_8;
	__le64 qw_sata_ncq15_12;
	__le64 qw_sata_ncq19_16;
	__le64 qw_sata_ncq23_20;
	__le64 qw_sata_ncq27_24;
	__le64 qw_sata_ncq31_28;
	__le64 qw_non_ncq_iptt;
	__le64 qw_rsvd0;
	__le64 qw_rsvd1;
};

struct hisi_sas_iost {
	__le64 qw0;
	__le64 qw1;
	__le64 qw2;
	__le64 qw3;
};

struct hisi_sas_err_record {
	/* dw0 */
	__le32 dma_err_type;

	/* dw1 */
	__le32 trans_tx_fail_type;

	/* dw2 */
	__le32 trans_rx_fail_type;

	/* dw3 */
	u32 rsvd;
};

struct hisi_sas_initial_fis {
	struct hisi_sas_err_record err_record;
	struct dev_to_host_fis fis;
	u32 rsvd[3];
};

struct hisi_sas_breakpoint {
	u8	data[128];	/*io128 byte*/
};

struct hisi_sas_sge {
	__le32 addr_lo;
	__le32 addr_hi;
	__le32 page_ctrl_0;
	__le32 page_ctrl_1;
	__le32 data_len;
	__le32 data_off;
};

struct hisi_sas_command_table_smp {
	u8 bytes[44];
};

struct hisi_sas_command_table_stp {
	struct	host_to_dev_fis command_fis;
	u8	dummy[12];
	u8	atapi_cdb[ATAPI_CDB_LEN];
};

#define HISI_SAS_SGE_PAGE_CNT SCSI_MAX_SG_SEGMENTS
struct hisi_sas_sge_page {
	struct hisi_sas_sge sge[HISI_SAS_SGE_PAGE_CNT];
};

struct hisi_sas_command_table_ssp {
	struct ssp_frame_hdr hdr;
	union {
		struct {
			struct ssp_command_iu task;
			u32 prot[6];
		};
		struct ssp_tmf_iu ssp_task;
		struct xfer_rdy_iu xfer_rdy;
		struct ssp_response_iu ssp_res;
	} u;
};

union hisi_sas_command_table {
	struct hisi_sas_command_table_ssp ssp;
	struct hisi_sas_command_table_smp smp;
	struct hisi_sas_command_table_stp stp;
};

int hisi_sas_scan_finished(struct Scsi_Host *shost, unsigned long time);
void hisi_sas_scan_start(struct Scsi_Host *shost);

void hisi_sas_slot_index_init(struct hisi_hba *hisi_hba);
void hisi_sas_phy_init(struct hisi_hba *hisi_hba, int i);
int hisi_sas_dev_found(struct domain_device *dev);
void hisi_sas_dev_gone(struct domain_device *dev);
int hisi_sas_queue_command(struct sas_task *task, gfp_t gfp_flags);
int hisi_sas_abort_task(struct sas_task *task);
int hisi_sas_abort_task_set(struct domain_device *dev, u8 *lun);
int hisi_sas_clear_aca(struct domain_device *dev, u8 *lun);
int hisi_sas_clear_task_set(struct domain_device *dev, u8 *lun);
int hisi_sas_I_T_nexus_reset(struct domain_device *dev);
int hisi_sas_lu_reset(struct domain_device *dev, u8 *lun);
int hisi_sas_query_task(struct sas_task *task);
void hisi_sas_port_formed(struct asd_sas_phy *sas_phy);
void hisi_sas_port_deformed(struct asd_sas_phy *sas_phy);
void hisi_sas_phy_down(struct hisi_hba *hisi_hba, int phy_no, int rdy);
void hisi_sas_wq_process(struct work_struct *work);
void hisi_sas_slot_task_free(struct hisi_hba *hisi_hba, struct sas_task *task,
			struct hisi_sas_slot *slot);

/* hw specific functions */
extern int slot_complete_v1_hw(struct hisi_hba *hisi_hba,
			       struct hisi_sas_slot *slot,
			       int abort);
extern void hisi_sas_setup_itct_v1_hw(struct hisi_hba *hisi_hba,
				      struct hisi_sas_device *device);
extern void start_delivery_v1_hw(struct hisi_hba *hisi_hba);
extern int get_free_slot_v1_hw(struct hisi_hba *hisi_hba, int *q, int *s);
extern int prep_ssp_v1_hw(struct hisi_hba *hisi_hba,
			  struct hisi_sas_tei *tei, int is_tmf,
			  struct hisi_sas_tmf_task *tmf);
extern int prep_smp_v1_hw(struct hisi_hba *hisi_hba,
			  struct hisi_sas_tei *tei);
extern int interrupt_init_v1_hw(struct hisi_hba *hisi_hba);
extern int interrupt_openall_v1_hw(struct hisi_hba *hisi_hba);
extern int hw_init_v1_hw(struct hisi_hba *hisi_hba);
extern int free_device_v1_hw(struct hisi_hba *hisi_hba,
			     struct hisi_sas_device *dev);
extern int phys_init_v1_hw(struct hisi_hba *hisi_hba);
extern void sl_notify_v1_hw(struct hisi_hba *hisi_hba, int phy_no);
extern void setup_itct_v1_hw(struct hisi_hba *hisi_hba,
			     struct hisi_sas_device *device);
#endif
