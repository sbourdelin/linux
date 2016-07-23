/*
 * Copyright (c) 2016 Avago Technologies.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED, EXCEPT TO
 * THE EXTENT THAT SUCH DISCLAIMERS ARE HELD TO BE LEGALLY INVALID.
 * See the GNU General Public License for more details, a copy of which
 * can be found in the file COPYING included with this package
 *
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/blk-mq.h>
#include <linux/parser.h>

#include "nvmet.h"
#include <linux/nvme-fc-driver.h>
#include <linux/nvme-fc.h>


/* *************************** Data Structures/Defines ****************** */


#define NVMET_LS_CTX_COUNT		4

/* for this implementation, assume small single frame rqst/rsp */
#define NVME_FC_MAX_LS_BUFFER_SIZE		2048

struct nvmet_fc_tgtport;
struct nvmet_fc_tgt_assoc;

struct nvmet_fc_ls_iod {
	struct nvmefc_tgt_ls_req	*lsreq;

	struct list_head		ls_list;	/* tgtport->ls_list */

	struct nvmet_fc_tgtport		*tgtport;
	struct nvmet_fc_tgt_assoc	*assoc;

	u8				*rqstbuf;
	u8				*rspbuf;
	u16				rqstdatalen;
	dma_addr_t			rspdma;

	struct scatterlist		sg[2];

	struct work_struct		work;
} __aligned(sizeof(unsigned long long));

#define NVMET_FC_MAX_KB_PER_XFR		256

enum nvmet_fcp_datadir {
	NVMET_FCP_NODATA,
	NVMET_FCP_WRITE,
	NVMET_FCP_READ,
	NVMET_FCP_ABORTED,
};

struct nvmet_fc_fcp_iod {
	struct nvmefc_tgt_fcp_req	*fcpreq;

	struct nvme_fc_cmd_iu		cmdiubuf;
	struct nvme_fc_ersp_iu		rspiubuf;
	dma_addr_t			rspdma;
	struct scatterlist		*data_sg;
	struct scatterlist		*next_sg;
	int				data_sg_cnt;
	u32				next_sg_offset;
	u32				total_length;
	u32				offset;
	enum nvmet_fcp_datadir		io_dir;
	bool				aborted;

	struct nvmet_req		req;
	struct work_struct		work;

	struct nvmet_fc_tgtport		*tgtport;
	struct nvmet_fc_tgt_queue	*queue;

	struct list_head		fcp_list;	/* tgtport->fcp_list */
};

struct nvmet_fc_tgtport {

	struct nvmet_fc_target_port	fc_target_port;

	struct list_head		tgt_list; /* nvme_fc_target_list */
	struct device			*dev;	/* dev for dma mapping */
	struct nvmet_fc_target_template	*ops;

	struct nvmet_fc_ls_iod		*iod;
	spinlock_t			lock;
	struct list_head		ls_list;
	struct list_head		ls_busylist;
	struct list_head		assoc_list;
	u32				assoc_cnt;
	struct nvmet_port		*port;

	struct kref			ref;
};

struct nvmet_fc_tgt_queue {
	bool				connected;
	bool				ninetypercent;
	u16				qid;
	u16				sqsize;
	u16				ersp_ratio;
	u16				sqhd;
	atomic_t			sqtail;
	atomic_t			zrspcnt;
	atomic_t			rsn;
	struct nvmet_port		*port;
	struct nvmet_cq			nvme_cq;
	struct nvmet_sq			nvme_sq;
	struct nvmet_fc_tgt_assoc	*assoc;
	struct nvmet_fc_fcp_iod		*fod;		/* array of fcp_iods */
	struct list_head		fod_list;
	struct workqueue_struct		*work_q;
} __aligned(sizeof(unsigned long long));

struct nvmet_fc_tgt_assoc {
	u64				association_id;
	u32				a_id;
	struct nvmet_fc_tgtport		*tgtport;
	struct list_head		a_list;
	struct nvmet_fc_tgt_queue	*queues[NVMET_NR_QUEUES];
};


static inline int
nvmet_fc_iodnum(struct nvmet_fc_ls_iod *iodptr)
{
	return (iodptr - iodptr->tgtport->iod);
}

static inline int
nvmet_fc_fodnum(struct nvmet_fc_fcp_iod *fodptr)
{
	return (fodptr - fodptr->queue->fod);
}


#define NVMET_FC_QUEUEID_MASK		((u64)(NVMET_NR_QUEUES-1))
						/* stuff qid into lower bits */

static inline u64
nvmet_fc_makeconnid(struct nvmet_fc_tgt_assoc *assoc, u16 qid)
{
	return (u64)(((u64)(assoc) & ~NVMET_FC_QUEUEID_MASK) | qid);
}

static inline u64
nvmet_fc_getassociationid(u64 connectionid)
{
	return (u64)((connectionid) & ~NVMET_FC_QUEUEID_MASK);
}

static inline u16
nvmet_fc_getqueueid(u64 connectionid)
{
	return (u16)((connectionid) & NVMET_FC_QUEUEID_MASK);
}


/* *************************** Globals **************************** */


static DEFINE_SPINLOCK(nvme_fc_tgtlock);

static LIST_HEAD(nvmet_fc_target_list);
static u32 nvmet_fc_tgtport_cnt;


static void nvmet_fc_handle_ls_rqst_work(struct work_struct *work);
static void nvmet_fc_handle_fcp_rqst_work(struct work_struct *work);


/* *********************** FC-NVME Port Management ************************ */


static int
nvmet_fc_alloc_ls_iodlist(struct nvmet_fc_tgtport *tgtport)
{
	struct nvmet_fc_ls_iod *iod;
	int i;

	iod = kcalloc(NVMET_LS_CTX_COUNT, sizeof(struct nvmet_fc_ls_iod),
			GFP_KERNEL);
	if (!iod)
		return -ENOMEM;

	tgtport->iod = iod;

	for (i = 0; i < NVMET_LS_CTX_COUNT; iod++, i++) {
		INIT_WORK(&iod->work, nvmet_fc_handle_ls_rqst_work);
		iod->tgtport = tgtport;
		list_add_tail(&iod->ls_list, &tgtport->ls_list);

		iod->rqstbuf = kcalloc(2, NVME_FC_MAX_LS_BUFFER_SIZE,
			GFP_KERNEL);
		if (!iod->rqstbuf)
			goto out_fail;

		iod->rspbuf = iod->rqstbuf + NVME_FC_MAX_LS_BUFFER_SIZE;

		/* TODO: better to use dma_map_page() ?*/
		iod->rspdma = dma_map_single(tgtport->dev, iod->rspbuf,
						NVME_FC_MAX_LS_BUFFER_SIZE,
						DMA_TO_DEVICE);
		if (dma_mapping_error(tgtport->dev, iod->rspdma))
			goto out_fail;
	}

	return 0;

out_fail:
	kfree(iod->rqstbuf);
	list_del(&iod->ls_list);
	for (iod--, i--; i >= 0; iod--, i--) {
		dma_unmap_single(tgtport->dev,
			iod->rspdma, NVME_FC_MAX_LS_BUFFER_SIZE, DMA_TO_DEVICE);
		kfree(iod->rqstbuf);
		list_del(&iod->ls_list);
	}

	kfree(iod);

	return -EFAULT;
}

static void
nvmet_fc_free_ls_iodlist(struct nvmet_fc_tgtport *tgtport)
{
	struct nvmet_fc_ls_iod *iod = tgtport->iod;
	int i;

	for (i = 0; i < NVMET_LS_CTX_COUNT; iod++, i++) {
		dma_unmap_single(tgtport->dev,
				iod->rspdma, NVME_FC_MAX_LS_BUFFER_SIZE,
				DMA_TO_DEVICE);
		kfree(iod->rqstbuf);
		list_del(&iod->ls_list);
	}
	kfree(tgtport->iod);
}

static struct nvmet_fc_ls_iod *
nvmet_fc_alloc_ls_iod(struct nvmet_fc_tgtport *tgtport)
{
	static struct nvmet_fc_ls_iod *iod;
	unsigned long flags;

	spin_lock_irqsave(&tgtport->lock, flags);
	iod = list_first_entry_or_null(&tgtport->ls_list,
					struct nvmet_fc_ls_iod, ls_list);
	if (iod)
		list_move_tail(&iod->ls_list, &tgtport->ls_busylist);
	spin_unlock_irqrestore(&tgtport->lock, flags);
	return iod;
}


static void
nvmet_fc_free_ls_iod(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_ls_iod *iod)
{
	unsigned long flags;

	spin_lock_irqsave(&tgtport->lock, flags);
	list_move(&iod->ls_list, &tgtport->ls_list);
	spin_unlock_irqrestore(&tgtport->lock, flags);
}

static void
nvmet_fc_prep_fcp_iodlist(struct nvmet_fc_tgtport *tgtport,
				struct nvmet_fc_tgt_queue *queue)
{
	struct nvmet_fc_fcp_iod *fod = queue->fod;
	int i;

	for (i = 0; i < queue->sqsize; fod++, i++) {
		INIT_WORK(&fod->work, nvmet_fc_handle_fcp_rqst_work);
		fod->tgtport = tgtport;
		fod->queue = queue;
		list_add_tail(&fod->fcp_list, &queue->fod_list);

		/* TODO: better to use dma_map_page() ?*/
		fod->rspdma = dma_map_single(
					tgtport->dev, &fod->rspiubuf,
					sizeof(fod->rspiubuf), DMA_TO_DEVICE);
		if (dma_mapping_error(tgtport->dev, fod->rspdma)) {
			list_del(&fod->fcp_list);
			for (fod--, i--; i >= 0; fod--, i--) {
				dma_unmap_single(tgtport->dev,
					fod->rspdma, sizeof(fod->rspiubuf),
					DMA_TO_DEVICE);
				fod->rspdma = 0L;
				list_del(&fod->fcp_list);
			}

			return;
		}
	}
}

static void
nvmet_fc_destroy_fcp_iodlist(struct nvmet_fc_tgtport *tgtport,
				struct nvmet_fc_tgt_queue *queue)
{
	struct nvmet_fc_fcp_iod *fod = queue->fod;
	int i;

	for (i = 0; i < queue->sqsize; fod++, i++) {
		if (fod->rspdma)
			dma_unmap_single(tgtport->dev, fod->rspdma,
				sizeof(fod->rspiubuf), DMA_TO_DEVICE);
	}
}

static struct nvmet_fc_fcp_iod *
nvmet_fc_alloc_fcp_iod(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_tgt_queue *queue)
{
	static struct nvmet_fc_fcp_iod *fod;
	unsigned long flags;

	spin_lock_irqsave(&tgtport->lock, flags);
	fod = list_first_entry_or_null(&queue->fod_list,
					struct nvmet_fc_fcp_iod, fcp_list);
	if (fod)
		list_del(&fod->fcp_list);
	spin_unlock_irqrestore(&tgtport->lock, flags);
	return fod;
}


static void
nvmet_fc_free_fcp_iod(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_fcp_iod *fod)
{
	unsigned long flags;

	spin_lock_irqsave(&tgtport->lock, flags);
	list_add_tail(&fod->fcp_list, &fod->queue->fod_list);
	spin_unlock_irqrestore(&tgtport->lock, flags);
}

static struct nvmet_fc_tgt_queue *
nvmet_fc_alloc_target_queue(struct nvmet_fc_tgt_assoc *assoc,
			u16 qid, u16 sqsize)
{
	struct nvmet_fc_tgt_queue *queue;
	unsigned long flags;
	int ret;

	if (qid >= NVMET_NR_QUEUES)
		return NULL;

	queue = kzalloc((sizeof(*queue) +
				(sizeof(struct nvmet_fc_fcp_iod) * sqsize)),
				GFP_KERNEL);
	if (!queue)
		return NULL;

	queue->work_q = alloc_workqueue("ntfc%d.%d.%d", 0, 0,
				assoc->tgtport->fc_target_port.port_num,
				assoc->a_id, qid);
	if (!queue->work_q) {
		kfree(queue);
		return NULL;
	}

	queue->fod = (struct nvmet_fc_fcp_iod *)&queue[1];
	queue->qid = qid;
	queue->sqsize = sqsize;
	queue->assoc = assoc;
	queue->connected = false;
	queue->port = assoc->tgtport->port;
	INIT_LIST_HEAD(&queue->fod_list);
	atomic_set(&queue->sqtail, 0);
	atomic_set(&queue->rsn, 1);
	atomic_set(&queue->zrspcnt, 0);

	nvmet_fc_prep_fcp_iodlist(assoc->tgtport, queue);

	ret = nvmet_sq_init(&queue->nvme_sq);
	if (ret) {
		nvmet_fc_destroy_fcp_iodlist(assoc->tgtport, queue);
		destroy_workqueue(queue->work_q);
		kfree(queue);
		return NULL;
	}

	BUG_ON(assoc->queues[qid]);
	spin_lock_irqsave(&assoc->tgtport->lock, flags);
	assoc->queues[qid] = queue;
	spin_unlock_irqrestore(&assoc->tgtport->lock, flags);

	return queue;
}

static void
nvmet_fc_free_target_queue(struct nvmet_fc_tgt_queue *queue)
{
	struct nvmet_fc_tgtport *tgtport = queue->assoc->tgtport;
	unsigned long flags;

	/*
	 * beware: nvmet layer hangs waiting for a completion if
	 * connect command failed
	 */
	flush_workqueue(queue->work_q);
	if (queue->connected)
		nvmet_sq_destroy(&queue->nvme_sq);
	spin_lock_irqsave(&tgtport->lock, flags);
	queue->assoc->queues[queue->qid] = NULL;
	spin_unlock_irqrestore(&tgtport->lock, flags);
	nvmet_fc_destroy_fcp_iodlist(tgtport, queue);
	destroy_workqueue(queue->work_q);
	kfree(queue);
}

static struct nvmet_fc_tgt_queue *
nvmet_fc_find_target_queue(struct nvmet_fc_tgtport *tgtport,
				u64 connection_id)
{
	struct nvmet_fc_tgt_assoc *assoc;
	u64 association_id = nvmet_fc_getassociationid(connection_id);
	u16 qid = nvmet_fc_getqueueid(connection_id);
	unsigned long flags;

	spin_lock_irqsave(&tgtport->lock, flags);
	list_for_each_entry(assoc, &tgtport->assoc_list, a_list) {
		if (association_id == assoc->association_id) {
			spin_unlock_irqrestore(&tgtport->lock, flags);
			return assoc->queues[qid];
		}
	}
	spin_unlock_irqrestore(&tgtport->lock, flags);
	return NULL;
}

static struct nvmet_fc_tgt_assoc *
nvmet_fc_alloc_target_assoc(struct nvmet_fc_tgtport *tgtport)
{
	struct nvmet_fc_tgt_assoc *assoc;
	unsigned long flags;

	assoc = kzalloc(sizeof(*assoc), GFP_KERNEL);
	if (!assoc)
		return NULL;

	assoc->tgtport = tgtport;
	assoc->association_id = cpu_to_le64(nvmet_fc_makeconnid(assoc, 0));
	INIT_LIST_HEAD(&assoc->a_list);

	spin_lock_irqsave(&tgtport->lock, flags);
	assoc->a_id = tgtport->assoc_cnt++;
	list_add_tail(&assoc->a_list, &tgtport->assoc_list);
	spin_unlock_irqrestore(&tgtport->lock, flags);

	return assoc;
}

static void
nvmet_fc_free_target_assoc(struct nvmet_fc_tgt_assoc *assoc)
{
	struct nvmet_fc_tgtport *tgtport = assoc->tgtport;
	unsigned long flags;

	spin_lock_irqsave(&tgtport->lock, flags);
	list_del(&assoc->a_list);
	spin_unlock_irqrestore(&tgtport->lock, flags);
	kfree(assoc);
}

static struct nvmet_fc_tgt_assoc *
nvmet_fc_find_target_assoc(struct nvmet_fc_tgtport *tgtport,
				u64 association_id)
{
	struct nvmet_fc_tgt_assoc *assoc;
	struct nvmet_fc_tgt_assoc *ret = NULL;
	unsigned long flags;

	spin_lock_irqsave(&tgtport->lock, flags);
	list_for_each_entry(assoc, &tgtport->assoc_list, a_list) {
		if (association_id == assoc->association_id) {
			ret = assoc;
			break;
		}
	}
	spin_unlock_irqrestore(&tgtport->lock, flags);

	return ret;
}


/**
 * nvme_fc_register_targetport - transport entry point called by an
 *                              LLDD to register the existence of a local
 *                              NVME subystem FC port.
 * @pinfo:     pointer to information about the port to be registered
 * @template:  LLDD entrypoints and operational parameters for the port
 * @dev:       physical hardware device node port corresponds to. Will be
 *             used for DMA mappings
 * @tgtport_p:   pointer to a local port pointer. Upon success, the routine
 *             will allocate a nvme_fc_local_port structure and place its
 *             address in the local port pointer. Upon failure, local port
 *             pointer will be set to 0.
 *
 * Returns:
 * a completion status. Must be 0 upon success; a negative errno
 * (ex: -ENXIO) upon failure.
 */
int
nvmet_fc_register_targetport(struct nvmet_fc_port_info *pinfo,
			struct nvmet_fc_target_template *template,
			struct device *dev,
			struct nvmet_fc_target_port **portptr)
{
	struct nvmet_fc_tgtport *newrec;
	unsigned long flags;
	int ret;

	if (!template->xmt_ls_rsp || !template->fcp_op ||
	    !template->max_hw_queues || !template->max_sgl_segments ||
	    !template->max_dif_sgl_segments || !template->dma_boundary) {
		ret = -EINVAL;
		goto out_regtgt_failed;
	}

	newrec = kzalloc((sizeof(*newrec) + template->target_priv_sz),
			 GFP_KERNEL);
	if (!newrec) {
		ret = -ENOMEM;
		goto out_regtgt_failed;
	}

	newrec->fc_target_port.node_name = pinfo->node_name;
	newrec->fc_target_port.port_name = pinfo->port_name;
	newrec->fc_target_port.private = &newrec[1];
	newrec->fc_target_port.port_id = pinfo->port_id;
	newrec->fc_target_port.fabric_name = pinfo->fabric_name;
	INIT_LIST_HEAD(&newrec->tgt_list);
	newrec->dev = dev;
	newrec->ops = template;
	spin_lock_init(&newrec->lock);
	INIT_LIST_HEAD(&newrec->ls_list);
	INIT_LIST_HEAD(&newrec->ls_busylist);
	INIT_LIST_HEAD(&newrec->assoc_list);

	ret = nvmet_fc_alloc_ls_iodlist(newrec);
	if (ret) {
		ret = -ENOMEM;
		goto out_free_newrec;
	}

	spin_lock_irqsave(&nvme_fc_tgtlock, flags);
	newrec->fc_target_port.port_num = nvmet_fc_tgtport_cnt++;
	list_add_tail(&newrec->tgt_list, &nvmet_fc_target_list);
	spin_unlock_irqrestore(&nvme_fc_tgtlock, flags);

	*portptr = &newrec->fc_target_port;
	return 0;

out_free_newrec:
	kfree(newrec);
out_regtgt_failed:
	*portptr = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(nvmet_fc_register_targetport);


/**
 * nvme_fc_unregister_targetport - transport entry point called by an
 *                              LLDD to deregister/remove a previously
 *                              registered a local NVME subsystem FC port.
 * @tgtport: pointer to the (registered) target port that is to be
 *           deregistered.
 *
 * Returns:
 * a completion status. Must be 0 upon success; a negative errno
 * (ex: -ENXIO) upon failure.
 */
int
nvmet_fc_unregister_targetport(struct nvmet_fc_target_port *target_port)
{
	struct nvmet_fc_tgtport *tgtport =
		container_of(target_port, struct nvmet_fc_tgtport,
				 fc_target_port);
	unsigned long flags;
	u32 pnum;

	pnum = tgtport->fc_target_port.port_num;

	spin_lock_irqsave(&nvme_fc_tgtlock, flags);
	list_del(&tgtport->tgt_list);
	spin_unlock_irqrestore(&nvme_fc_tgtlock, flags);

	nvmet_fc_free_ls_iodlist(tgtport);
	kfree(tgtport);
	return 0;
}
EXPORT_SYMBOL_GPL(nvmet_fc_unregister_targetport);


static void
__nvmet_fc_free_queues(struct nvmet_fc_tgt_assoc *assoc)
{
	struct nvmet_fc_tgt_queue *queue;
	int i;

	for (i = 0; i < NVMET_NR_QUEUES; i++) {
		queue = assoc->queues[i];
		if (queue) {
			assoc->queues[i] = NULL;
			kfree(queue);
		}
	}
}

static void
__nvmet_fc_free_assocs(struct nvmet_fc_tgtport *tgtport)
{
	struct nvmet_fc_tgt_assoc *assoc, *next;

	list_for_each_entry_safe(assoc, next,
				&tgtport->assoc_list, a_list) {
		list_del(&assoc->a_list);
		__nvmet_fc_free_queues(assoc);
		kfree(assoc);
	}
}

static void
__nvmet_fc_free_tgtports(void)
{
	struct nvmet_fc_tgtport *tgtport, *next;
	unsigned long flags;

	spin_lock_irqsave(&nvme_fc_tgtlock, flags);
	list_for_each_entry_safe(tgtport, next, &nvmet_fc_target_list,
			tgt_list) {
		list_del(&tgtport->tgt_list);
		__nvmet_fc_free_assocs(tgtport);
		kfree(tgtport);
	}
	spin_unlock_irqrestore(&nvme_fc_tgtlock, flags);
}



/* *********************** FC-NVME LS Handling **************************** */


static void
nvmet_fc_format_rsp_hdr(void *buf, u8 ls_cmd, u32 desc_len, u8 rqst_ls_cmd)
{
	struct fcnvme_ls_acc_hdr *acc = buf;

	acc->w0.ls_cmd = ls_cmd;
	acc->desc_list_len = desc_len;
	acc->rqst.desc_tag = cpu_to_be32(FCNVME_LSDESC_RQST);
	acc->rqst.desc_len =
			FCNVME_LSDESC_LEN(struct fcnvme_lsdesc_rqst);
	acc->rqst.w0.ls_cmd = rqst_ls_cmd;
}

static int
nvmet_fc_format_rjt(void *buf, u16 buflen, u8 ls_cmd,
			u8 reason, u8 explanation, u8 vendor)
{
	struct fcnvme_ls_rjt *rjt = buf;

	BUG_ON(buflen < sizeof(struct fcnvme_ls_rjt));
	nvmet_fc_format_rsp_hdr(buf, FCNVME_LSDESC_RQST,
			FCNVME_LSDESC_LEN(struct fcnvme_ls_rjt),
			ls_cmd);
	rjt->rjt.desc_tag = cpu_to_be32(FCNVME_LSDESC_RJT);
	rjt->rjt.desc_len = FCNVME_LSDESC_LEN(struct fcnvme_lsdesc_rjt);
	rjt->rjt.reason_code = reason;
	rjt->rjt.reason_explanation = explanation;
	rjt->rjt.vendor = vendor;

	return sizeof(struct fcnvme_ls_rjt);
}

/* Validation Error indexes into the string table below */
enum {
	VERR_NO_ERROR		= 0,
	VERR_CR_ASSOC_LEN	= 1,
	VERR_CR_ASSOC_RQST_LEN	= 2,
	VERR_CR_ASSOC_CMD	= 3,
	VERR_CR_ASSOC_CMD_LEN	= 4,
	VERR_ERSP_RATIO		= 5,
	VERR_ASSOC_ALLOC_FAIL	= 6,
	VERR_NO_ASSOC		= 7,
	VERR_QUEUE_ALLOC_FAIL	= 8,
	VERR_CR_CONN_LEN	= 9,
	VERR_CR_CONN_RQST_LEN	= 10,
	VERR_ASSOC_ID		= 11,
	VERR_ASSOC_ID_LEN	= 12,
	VERR_CR_CONN_CMD	= 13,
	VERR_CR_CONN_CMD_LEN	= 14,
	VERR_DISCONN_LEN	= 15,
	VERR_DISCONN_RQST_LEN	= 16,
	VERR_DISCONN_CMD	= 17,
	VERR_DISCONN_CMD_LEN	= 18,
	VERR_DISCONN_SCOPE	= 19,
};

static char *validation_errors[] = {
	"OK",
	"Bad CR_ASSOC Length",
	"Bad CR_ASSOC Rqst Length",
	"Not CR_ASSOC Cmd",
	"Bad CR_ASSOC Cmd Length",
	"Bad Ersp Ratio",
	"Association Allocation Failed",
	"No Association",
	"Queue Allocation Failed",
	"Bad CR_CONN Length",
	"Bad CR_CONN Rqst Length",
	"Not Association ID",
	"Bad Association ID Length",
	"Not CR_CONN Cmd",
	"Bad CR_CONN Cmd Length",
	"Bad DISCONN Length",
	"Bad DISCONN Rqst Length",
	"Not DISCONN Cmd",
	"Bad DISCONN Cmd Length",
	"Bad Disconnect Scope",
};

static void
nvmet_fc_ls_create_association(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_ls_iod *iod)
{
	struct fcnvme_ls_cr_assoc_rqst *rqst =
				(struct fcnvme_ls_cr_assoc_rqst *)iod->rqstbuf;
	struct fcnvme_ls_cr_assoc_acc *acc =
				(struct fcnvme_ls_cr_assoc_acc *)iod->rspbuf;
	struct nvmet_fc_tgt_queue *queue;
	int ret = 0;

	memset(acc, 0, sizeof(*acc));

	if (iod->rqstdatalen < sizeof(struct fcnvme_ls_cr_assoc_rqst))
		ret = VERR_CR_ASSOC_LEN;
	else if (rqst->desc_list_len !=
			FCNVME_LSDESC_LEN(struct fcnvme_ls_cr_assoc_rqst))
		ret = VERR_CR_ASSOC_RQST_LEN;
	else if (rqst->assoc_cmd.desc_tag !=
			cpu_to_be32(FCNVME_LSDESC_CREATE_ASSOC_CMD))
		ret = VERR_CR_ASSOC_CMD;
	else if (rqst->assoc_cmd.desc_len !=
			FCNVME_LSDESC_LEN(struct fcnvme_lsdesc_cr_assoc_cmd))
		ret = VERR_CR_ASSOC_CMD_LEN;
	else if (!rqst->assoc_cmd.ersp_ratio ||
		 (be16_to_cpu(rqst->assoc_cmd.ersp_ratio) >=
				be16_to_cpu(rqst->assoc_cmd.sqsize)))
		ret = VERR_ERSP_RATIO;

	else {
		/* new association w/ admin queue */
		iod->assoc = nvmet_fc_alloc_target_assoc(tgtport);
		if (!iod->assoc)
			ret = VERR_ASSOC_ALLOC_FAIL;
		else {
			queue = nvmet_fc_alloc_target_queue(iod->assoc, 0,
					be16_to_cpu(rqst->assoc_cmd.sqsize));
			if (!queue) {
				ret = VERR_QUEUE_ALLOC_FAIL;
				nvmet_fc_free_target_assoc(iod->assoc);
			}
		}
	}

	if (ret) {
		dev_err(tgtport->dev,
			"Create Association LS failed: %s\n",
			validation_errors[ret]);
		iod->lsreq->rsplen = nvmet_fc_format_rjt(acc,
				NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				LSRJT_REASON_LOGICAL_ERROR,
				LSRJT_EXPL_NO_EXPLANATION, 0);
		return;
	}

	queue->ersp_ratio = be16_to_cpu(rqst->assoc_cmd.ersp_ratio);
	queue->connected = true;
	queue->sqhd = 0;	/* TODO: best place to init value */

	/* format a response */

	iod->lsreq->rsplen = sizeof(*acc);

	nvmet_fc_format_rsp_hdr(acc, FCNVME_LS_ACC,
			FCNVME_LSDESC_LEN(struct fcnvme_ls_cr_assoc_acc),
			FCNVME_LS_CREATE_ASSOCIATION);
	acc->associd.desc_tag = cpu_to_be32(FCNVME_LSDESC_ASSOC_ID);
	acc->associd.desc_len = FCNVME_LSDESC_LEN(
					struct fcnvme_lsdesc_assoc_id);
	acc->associd.association_id =
			cpu_to_be64(nvmet_fc_makeconnid(iod->assoc, 0));
	acc->connectid.desc_tag = cpu_to_be32(FCNVME_LSDESC_CONN_ID);
	acc->connectid.desc_len = FCNVME_LSDESC_LEN(
					struct fcnvme_lsdesc_conn_id);
	acc->connectid.connection_id = acc->associd.association_id;
}

static void
nvmet_fc_ls_create_connection(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_ls_iod *iod)
{
	struct fcnvme_ls_cr_conn_rqst *rqst =
				(struct fcnvme_ls_cr_conn_rqst *)iod->rqstbuf;
	struct fcnvme_ls_cr_conn_acc *acc =
				(struct fcnvme_ls_cr_conn_acc *)iod->rspbuf;
	struct nvmet_fc_tgt_queue *queue;
	int ret = 0;

	memset(acc, 0, sizeof(*acc));

	if (iod->rqstdatalen < sizeof(struct fcnvme_ls_cr_conn_rqst))
		ret = VERR_CR_CONN_LEN;
	else if (rqst->desc_list_len !=
			FCNVME_LSDESC_LEN(struct fcnvme_ls_cr_conn_rqst))
		ret = VERR_CR_CONN_RQST_LEN;
	else if (rqst->associd.desc_tag != cpu_to_be32(FCNVME_LSDESC_ASSOC_ID))
		ret = VERR_ASSOC_ID;
	else if (rqst->associd.desc_len !=
			FCNVME_LSDESC_LEN(struct fcnvme_lsdesc_assoc_id))
		ret = VERR_ASSOC_ID_LEN;
	else if (rqst->connect_cmd.desc_tag !=
			cpu_to_be32(FCNVME_LSDESC_CREATE_CONN_CMD))
		ret = VERR_CR_CONN_CMD;
	else if (rqst->connect_cmd.desc_len !=
			FCNVME_LSDESC_LEN(struct fcnvme_lsdesc_cr_conn_cmd))
		ret = VERR_CR_CONN_CMD_LEN;
	else if (!rqst->connect_cmd.ersp_ratio ||
		 (be16_to_cpu(rqst->connect_cmd.ersp_ratio) >=
				be16_to_cpu(rqst->connect_cmd.sqsize)))
		ret = VERR_ERSP_RATIO;

	else {
		/* new io queue */
		iod->assoc = nvmet_fc_find_target_assoc(tgtport,
				be64_to_cpu(rqst->associd.association_id));
		if (!iod->assoc)
			ret = VERR_NO_ASSOC;
		else {
			queue = nvmet_fc_alloc_target_queue(iod->assoc,
					be16_to_cpu(rqst->connect_cmd.qid),
					be16_to_cpu(rqst->connect_cmd.sqsize));
			if (!queue)
				ret = VERR_QUEUE_ALLOC_FAIL;
		}
	}

	if (ret) {
		dev_err(tgtport->dev,
			"Create Connection LS failed: %s\n",
			validation_errors[ret]);
		iod->lsreq->rsplen = nvmet_fc_format_rjt(acc,
				NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				(ret == 10) ? LSRJT_REASON_PROTOCOL_ERROR :
						LSRJT_REASON_LOGICAL_ERROR,
				LSRJT_EXPL_NO_EXPLANATION, 0);
		return;
	}

	queue->ersp_ratio = be16_to_cpu(rqst->connect_cmd.ersp_ratio);
	queue->connected = true;
	queue->sqhd = 0;	/* TODO: best place to init value */

	/* format a response */

	iod->lsreq->rsplen = sizeof(*acc);

	nvmet_fc_format_rsp_hdr(acc, FCNVME_LS_ACC,
			FCNVME_LSDESC_LEN(struct fcnvme_ls_cr_conn_acc),
			FCNVME_LS_CREATE_CONNECTION);
	acc->connectid.desc_tag = cpu_to_be32(FCNVME_LSDESC_CONN_ID);
	acc->connectid.desc_len = FCNVME_LSDESC_LEN(
					struct fcnvme_lsdesc_conn_id);
	acc->connectid.connection_id =
			cpu_to_be64(nvmet_fc_makeconnid(iod->assoc,
				be16_to_cpu(rqst->connect_cmd.qid)));
}

static void
nvmet_fc_ls_disconnect(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_ls_iod *iod)
{
	struct fcnvme_ls_disconnect_rqst *rqst =
			(struct fcnvme_ls_disconnect_rqst *)iod->rqstbuf;
	struct fcnvme_ls_disconnect_acc *acc =
			(struct fcnvme_ls_disconnect_acc *)iod->rspbuf;
	struct nvmet_fc_tgt_queue *queue;
	struct nvmet_fc_tgt_assoc *assoc;
	int ret = 0, i;
	bool del_assoc = true;

	memset(acc, 0, sizeof(*acc));

	if (iod->rqstdatalen < sizeof(struct fcnvme_ls_disconnect_rqst))
		ret = VERR_DISCONN_LEN;
	else if (rqst->desc_list_len !=
			FCNVME_LSDESC_LEN(struct fcnvme_ls_disconnect_rqst))
		ret = VERR_DISCONN_RQST_LEN;
	else if (rqst->associd.desc_tag != cpu_to_be32(FCNVME_LSDESC_ASSOC_ID))
		ret = VERR_ASSOC_ID;
	else if (rqst->associd.desc_len !=
			FCNVME_LSDESC_LEN(struct fcnvme_lsdesc_assoc_id))
		ret = VERR_ASSOC_ID_LEN;
	else if (rqst->discon_cmd.desc_tag !=
			cpu_to_be32(FCNVME_LSDESC_DISCONN_CMD))
		ret = VERR_DISCONN_CMD;
	else if (rqst->discon_cmd.desc_len !=
			FCNVME_LSDESC_LEN(struct fcnvme_lsdesc_disconn_cmd))
		ret = VERR_DISCONN_CMD_LEN;
	else if ((rqst->discon_cmd.scope != FCNVME_DISCONN_ASSOCIATION) &&
			(rqst->discon_cmd.scope != FCNVME_DISCONN_CONNECTION))
		ret = VERR_DISCONN_SCOPE;
	else {
		/* match an active association */
		assoc = nvmet_fc_find_target_assoc(tgtport,
				be64_to_cpu(rqst->associd.association_id));
		iod->assoc = assoc;
		if (!assoc)
			ret = VERR_NO_ASSOC;
	}

	if (ret) {
		dev_err(tgtport->dev,
			"Disconnect LS failed: %s\n",
			validation_errors[ret]);
		iod->lsreq->rsplen = nvmet_fc_format_rjt(acc,
				NVME_FC_MAX_LS_BUFFER_SIZE, rqst->w0.ls_cmd,
				(ret == 8) ? LSRJT_REASON_PROTOCOL_ERROR :
						LSRJT_REASON_LOGICAL_ERROR,
				LSRJT_EXPL_NO_EXPLANATION, 0);
		return;
	}

	/* format a response */

	iod->lsreq->rsplen = sizeof(*acc);

	nvmet_fc_format_rsp_hdr(acc, FCNVME_LS_ACC,
			FCNVME_LSDESC_LEN(struct fcnvme_ls_disconnect_acc),
			FCNVME_LS_DISCONNECT);


	if (rqst->discon_cmd.scope == FCNVME_DISCONN_CONNECTION) {
		queue = nvmet_fc_find_target_queue(tgtport,
					be64_to_cpu(rqst->discon_cmd.id));
		if (queue) {
			nvmet_fc_free_target_queue(queue);

			/* see if there are any more queues */
			for (i = 0; i < NVMET_NR_QUEUES; i++)
				if (assoc->queues[i])
					break;

			/*
			 * if tearing down admin queue or no more queues,
			 * fall thru to tear down the association.
			 */
			if ((queue->qid) && (i != NVMET_NR_QUEUES))
				del_assoc = false;
		}
	}

	if (del_assoc) {
		for (i = NVMET_NR_QUEUES - 1; i >= 0; i--)
			if (assoc->queues[i])
				nvmet_fc_free_target_queue(
						assoc->queues[i]);
		/*
		 * Don't send ABTS's - let host side do that
		 */
		nvmet_fc_free_target_assoc(assoc);
	}
}



/* *********************** NVME Ctrl Routines **************************** */


static void nvmet_fc_fcp_nvme_cmd_done(struct nvmet_req *nvme_req);

static struct nvmet_fabrics_ops nvmet_fc_tgt_fcp_ops;

static void
nvmet_fc_xmt_ls_rsp_done(struct nvmefc_tgt_ls_req *lsreq)
{
	struct nvmet_fc_ls_iod *iod = lsreq->nvmet_fc_private;
	struct nvmet_fc_tgtport *tgtport = iod->tgtport;

	dma_sync_single_for_cpu(tgtport->dev, iod->rspdma,
				NVME_FC_MAX_LS_BUFFER_SIZE, DMA_TO_DEVICE);
	nvmet_fc_free_ls_iod(tgtport, iod);
}

static void
nvmet_fc_xmt_ls_rsp(struct nvmet_fc_tgtport *tgtport,
				struct nvmet_fc_ls_iod *iod)
{
	int ret;

	dma_sync_single_for_device(tgtport->dev, iod->rspdma,
				  NVME_FC_MAX_LS_BUFFER_SIZE, DMA_TO_DEVICE);

	ret = tgtport->ops->xmt_ls_rsp(&tgtport->fc_target_port, iod->lsreq);
	if (ret)
		nvmet_fc_xmt_ls_rsp_done(iod->lsreq);
}

/*
 * Actual processing routine for received FC-NVME LS Requests from the LLD
 */
void
nvmet_fc_handle_ls_rqst(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_ls_iod *iod)
{
	struct fcnvme_ls_rqst_w0 *w0 =
			(struct fcnvme_ls_rqst_w0 *)iod->rqstbuf;

	iod->lsreq->nvmet_fc_private = iod;
	iod->lsreq->rspbuf = iod->rspbuf;
	iod->lsreq->rspdma = iod->rspdma;
	iod->lsreq->done = nvmet_fc_xmt_ls_rsp_done;
	/* Be preventative. handlers will later set to valid length */
	iod->lsreq->rsplen = 0;

	iod->assoc = NULL;

	/*
	 * handlers:
	 *   parse request input, set up nvmet req (cmd, rsp,  execute)
	 *   and format the LS response
	 * if non-zero returned, then no futher action taken on the LS
	 * if zero:
	 *   valid to call nvmet layer if execute routine set
	 *   iod->rspbuf contains ls response
	 */
	switch (w0->ls_cmd) {
	case FCNVME_LS_CREATE_ASSOCIATION:
		/* Creates Association and initial Admin Queue/Connection */
		nvmet_fc_ls_create_association(tgtport, iod);
		break;
	case FCNVME_LS_CREATE_CONNECTION:
		/* Creates an IO Queue/Connection */
		nvmet_fc_ls_create_connection(tgtport, iod);
		break;
	case FCNVME_LS_DISCONNECT:
		/* Terminate a Queue/Connection or the Association */
		nvmet_fc_ls_disconnect(tgtport, iod);
		break;
	default:
		iod->lsreq->rsplen = nvmet_fc_format_rjt(iod->rspbuf,
				NVME_FC_MAX_LS_BUFFER_SIZE, w0->ls_cmd,
				LSRJT_REASON_INVALID_ELS_CODE,
				LSRJT_EXPL_NO_EXPLANATION, 0);
	}

	nvmet_fc_xmt_ls_rsp(tgtport, iod);
}

/*
 * Actual processing routine for received FC-NVME LS Requests from the LLD
 */
void
nvmet_fc_handle_ls_rqst_work(struct work_struct *work)
{
	struct nvmet_fc_ls_iod *iod =
		container_of(work, struct nvmet_fc_ls_iod, work);
	struct nvmet_fc_tgtport *tgtport = iod->tgtport;

	nvmet_fc_handle_ls_rqst(tgtport, iod);
}


/**
 * nvmet_fc_rcv_ls_req - transport entry point called by an LLDD
 *                       upon the reception of a NVME LS request.
 *
 * The nvmet-fc layer will copy payload to an internal structure for
 * processing.  As such, upon completion of the routine, the LLDD may
 * immediately free/reuse the LS request buffer passed in the call.
 *
 * If this routine returns error, the lldd should abort the exchange.
 *
 * @tgtport:    pointer to the (registered) target port the LS was receive on.
 * @lsreq:      pointer to a lsreq request structure to be used to reference
 *              the exchange corresponding to the LS.
 * @lsreqbuf:   pointer to the buffer containing the LS Request
 * @lsreqbuf_len: length, in bytes, of the received LS request
 */
int
nvmet_fc_rcv_ls_req(struct nvmet_fc_target_port *target_port,
			struct nvmefc_tgt_ls_req *lsreq,
			void *lsreqbuf, u32 lsreqbuf_len)
{
	struct nvmet_fc_tgtport *tgtport = container_of(target_port,
			struct nvmet_fc_tgtport, fc_target_port);
	struct nvmet_fc_ls_iod *iod;

	if (lsreqbuf_len > NVME_FC_MAX_LS_BUFFER_SIZE)
		return -E2BIG;

	iod = nvmet_fc_alloc_ls_iod(tgtport);
	if (!iod)
		return -ENOENT;

	iod->lsreq = lsreq;
	memcpy(iod->rqstbuf, lsreqbuf, lsreqbuf_len);
	iod->rqstdatalen = lsreqbuf_len;

	schedule_work(&iod->work);

	return 0;
}
EXPORT_SYMBOL_GPL(nvmet_fc_rcv_ls_req);


/*
 * **********************
 * Start of FCP handling
 * **********************
 */

static int
nvmet_fc_alloc_tgt_pgs(struct nvmet_fc_fcp_iod *fod)
{
	struct scatterlist *sg;
	struct page *page;
	unsigned int nent;
	u32 page_len, length;
	int i = 0;

	length = fod->total_length;
	nent = DIV_ROUND_UP(length, PAGE_SIZE);
	sg = kmalloc_array(nent, sizeof(struct scatterlist), GFP_KERNEL);
	if (!sg)
		return NVME_SC_INTERNAL;

	/* TODO: convert to using sg pagelist */
	sg_init_table(sg, nent);

	while (length) {
		page_len = min_t(u32, length, PAGE_SIZE);

		page = alloc_page(GFP_KERNEL);
		if (!page)
			goto out_free_pages;

		sg_set_page(&sg[i], page, page_len, 0);
		length -= page_len;
		i++;
	}

	fod->data_sg = sg;
	fod->data_sg_cnt = nent;
	fod->data_sg_cnt = dma_map_sg(fod->tgtport->dev, sg, nent,
				((fod->io_dir == NVMET_FCP_WRITE) ?
					DMA_FROM_DEVICE : DMA_TO_DEVICE));
				/* note: write from initiator perspective */

	if ((fod->data_sg_cnt) &&
	    (fod->data_sg_cnt < fod->tgtport->ops->max_sgl_segments))
		return 0;

	if (fod->data_sg_cnt)
		dma_unmap_sg(fod->tgtport->dev, fod->data_sg, fod->data_sg_cnt,
				((fod->io_dir == NVMET_FCP_WRITE) ?
					DMA_FROM_DEVICE : DMA_TO_DEVICE));

out_free_pages:
	while (i > 0) {
		i--;
		__free_page(sg_page(&sg[i]));
	}
	kfree(sg);
	fod->data_sg = NULL;
	fod->data_sg_cnt = 0;
	return NVME_SC_INTERNAL;
}

static void
nvmet_fc_free_tgt_pgs(struct nvmet_fc_fcp_iod *fod)
{
	struct scatterlist *sg;
	int count;

	if (!fod->data_sg || !fod->data_sg_cnt)
		return;

	dma_unmap_sg(fod->tgtport->dev, fod->data_sg, fod->data_sg_cnt,
				((fod->io_dir == NVMET_FCP_WRITE) ?
					DMA_FROM_DEVICE : DMA_TO_DEVICE));
	for_each_sg(fod->data_sg, sg, fod->data_sg_cnt, count)
		__free_page(sg_page(sg));
	kfree(fod->data_sg);
}

static void
nvmet_fc_abort_op(struct nvmet_fc_tgtport *tgtport,
				struct nvmefc_tgt_fcp_req *fcpreq)
{
	int ret;

	fcpreq->op = NVMET_FCOP_ABORT;
	fcpreq->offset = 0;
	fcpreq->timeout = 0;
	fcpreq->transfer_length = 0;
	fcpreq->transferred_length = 0;
	fcpreq->fcp_error = 0;
	fcpreq->sg_cnt = 0;

	ret = tgtport->ops->fcp_op(&tgtport->fc_target_port, fcpreq);
	if (ret) {
		BUG_ON(1);
		/* should never reach here !! */
	}
}


static bool
queue_90percent_full(struct nvmet_fc_tgt_queue *q, u32 sqhd)
{
	u32 sqtail, used;

	/* egad, this is ugly. And sqtail is just a best guess */
	sqtail = atomic_read(&q->sqtail) % q->sqsize;

	used = (sqtail < sqhd) ? (sqtail + q->sqsize - sqhd) : (sqtail - sqhd);
	return ((used * 10) >= (((u32)(q->sqsize - 1) * 9)));
}

/*
 * Prep RSP payload.
 * May be a NVMET_FCOP_RSP or NVMET_FCOP_READDATA_RSP op
 */
static void
nvmet_fc_prep_fcp_rsp(struct nvmet_fc_tgtport *tgtport,
				struct nvmet_fc_fcp_iod *fod)
{
	struct nvme_fc_ersp_iu *ersp = &fod->rspiubuf;
	struct nvme_common_command *sqe = &fod->cmdiubuf.sqe.common;
	struct nvme_completion *cqe = &ersp->cqe;
	u32 *cqewd = (u32 *)cqe;
	bool send_ersp = false;
	u32 rsn, rspcnt;

	/*
	 * check to see if we can send a 0's rsp.
	 *   Note: to send a 0's response, the NVME-FC host transport will
	 *   recreate the CQE. The host transport knows: sq id, SQHD (last
	 *   seen in an ersp), and command_id. Thus it will create a
	 *   zero-filled CQE with those known fields filled in. Transport
	 *   must send an ersp for any condition where the cqe won't match
	 *   this.
	 *
	 * Here are the FC-NVME mandated cases where we must send an ersp:
	 *  every N responses, where N=ersp_ratio
	 *  force fabric commands to send ersp's (not in FC-NVME but good
	 *    practice)
	 *  normal cmds: any time status is non-zero, or status is zero
	 *     but words 0 or 1 are non-zero.
	 *  the SQ is 90% or more full
	 *  the cmd is a fused command
	 */
	rspcnt = atomic_inc_return(&fod->queue->zrspcnt);
	if (!(rspcnt % fod->queue->ersp_ratio) ||
	    (sqe->opcode == nvme_fabrics_command) ||
	    (le16_to_cpu(cqe->status) & 0xFFFE) || cqewd[0] || cqewd[1] ||
	    (sqe->flags & (NVME_CMD_FUSE_FIRST | NVME_CMD_FUSE_SECOND)) ||
	    queue_90percent_full(fod->queue, cqe->sq_head))
		send_ersp = true;

	/* re-set the fields */
	fod->fcpreq->rspaddr = ersp;
	fod->fcpreq->rspdma = fod->rspdma;

	if (!send_ersp) {
		memset(ersp, 0, NVME_FC_SIZEOF_ZEROS_RSP);
		fod->fcpreq->rsplen = NVME_FC_SIZEOF_ZEROS_RSP;
	} else {
		ersp->iu_len = cpu_to_be16(sizeof(*ersp)/sizeof(u32));
		rsn = atomic_inc_return(&fod->queue->rsn);
		ersp->rsn = cpu_to_be32(rsn);
		fod->fcpreq->rsplen = sizeof(*ersp);
	}

	dma_sync_single_for_device(tgtport->dev, fod->rspdma,
				  sizeof(fod->rspiubuf), DMA_TO_DEVICE);
}

static void nvmet_fc_xmt_fcp_op_done(struct nvmefc_tgt_fcp_req *fcpreq);

static void
nvmet_fc_xmt_fcp_rsp(struct nvmet_fc_tgtport *tgtport,
				struct nvmet_fc_fcp_iod *fod)
{
	int ret;

	fod->fcpreq->op = NVMET_FCOP_RSP;
	fod->fcpreq->offset = 0;
	fod->fcpreq->timeout = 0;

	nvmet_fc_prep_fcp_rsp(tgtport, fod);

	ret = tgtport->ops->fcp_op(&tgtport->fc_target_port, fod->fcpreq);
	if (ret) {
		fod->aborted = true;
		nvmet_fc_abort_op(tgtport, fod->fcpreq);
	}
}

static void
nvmet_fc_transfer_fcp_data(struct nvmet_fc_tgtport *tgtport,
				struct nvmet_fc_fcp_iod *fod, u8 op)
{
	struct nvmefc_tgt_fcp_req *fcpreq = fod->fcpreq;
	struct scatterlist *sg, *datasg;
	u32 tlen, sg_off;
	int ret;

	fcpreq->op = op;
	fcpreq->offset = fod->offset;
	fcpreq->timeout = NVME_FC_TGTOP_TIMEOUT_SEC;
	tlen = min_t(u32, (NVMET_FC_MAX_KB_PER_XFR * 1024),
			(fod->total_length - fod->offset));
	tlen = min_t(u32, tlen, (NVME_FC_MAX_SEGMENTS * PAGE_SIZE));
	fcpreq->transfer_length = tlen;
	fcpreq->transferred_length = 0;
	fcpreq->fcp_error = 0;
	fcpreq->rsplen = 0;

	fcpreq->sg_cnt = 0;

	datasg = fod->next_sg;
	sg_off = fod->next_sg_offset;

	for (sg = fcpreq->sg ; tlen; sg++) {
		*sg = *datasg;
		if (sg_off) {
			sg->offset += sg_off;
			sg->length -= sg_off;
			sg->dma_address += sg_off;
			sg_off = 0;
		}
		if (tlen < sg->length) {
			sg->length = tlen;
			fod->next_sg = datasg;
			fod->next_sg_offset += tlen;
		} else if (tlen == sg->length) {
			fod->next_sg_offset = 0;
			fod->next_sg = sg_next(datasg);
		} else {
			fod->next_sg_offset = 0;
			datasg = sg_next(datasg);
		}
		tlen -= sg->length;
		fcpreq->sg_cnt++;
	}

	/*
	 * If the last READDATA request: check if LLDD supports
	 * combined xfr with response.
	 */
	if ((op == NVMET_FCOP_READDATA) &&
	    ((fod->offset + fcpreq->transfer_length) == fod->total_length) &&
	    (tgtport->ops->target_features & NVMET_FCTGTFEAT_READDATA_RSP)) {
		fcpreq->op = NVMET_FCOP_READDATA_RSP;
		nvmet_fc_prep_fcp_rsp(tgtport, fod);
	}

	ret = tgtport->ops->fcp_op(&tgtport->fc_target_port, fod->fcpreq);
	if (ret) {
		if (op == NVMET_FCOP_WRITEDATA)
			nvmet_req_complete(&fod->req, ret);
		else /* NVMET_FCOP_READDATA or NVMET_FCOP_READDATA_RSP */ {
			fcpreq->fcp_error = ret;
			fcpreq->transferred_length = 0;
			fod->aborted = true;
			nvmet_fc_xmt_fcp_op_done(fod->fcpreq);
		}
	}
}

static void
nvmet_fc_xmt_fcp_op_done(struct nvmefc_tgt_fcp_req *fcpreq)
{
	struct nvmet_fc_fcp_iod *fod = fcpreq->nvmet_fc_private;
	struct nvmet_fc_tgtport *tgtport = fod->tgtport;
	struct nvme_fc_ersp_iu *ersp = &fod->rspiubuf;
	struct nvme_completion *cqe = &ersp->cqe;

	switch (fcpreq->op) {

	case NVMET_FCOP_WRITEDATA:
		if (fcpreq->fcp_error) {
			nvmet_req_complete(&fod->req, fcpreq->fcp_error);
			return;
		}
		if (fcpreq->transferred_length != fcpreq->transfer_length) {
			nvmet_req_complete(&fod->req,
					NVME_SC_FC_TRANSPORT_ERROR);
			return;
		}

		fod->offset += fcpreq->transferred_length;
		if (fod->offset != fod->total_length) {
			/* transfer the next chunk */
			nvmet_fc_transfer_fcp_data(tgtport, fod,
						NVMET_FCOP_WRITEDATA);
			return;
		}

		/* data transfer complete, resume with nvmet layer */

		fod->req.execute(&fod->req);

		break;

	case NVMET_FCOP_READDATA:
		if (fcpreq->fcp_error)
			/* overwrite the nvmet status */
			cqe->status = cpu_to_le16(fcpreq->fcp_error);

		else if (fcpreq->transferred_length !=
					fcpreq->transfer_length)
			/* overwrite the nvmet status */
			cqe->status = cpu_to_le16(NVME_SC_FC_TRANSPORT_ERROR);

		else {
			fod->offset += fcpreq->transferred_length;
			if (fod->offset != fod->total_length) {
				/* transfer the next chunk */
				nvmet_fc_transfer_fcp_data(tgtport, fod,
							NVMET_FCOP_READDATA);
				return;
			}
		}

		/* data transfer complete, send response */

		/* data no longer needed */
		nvmet_fc_free_tgt_pgs(fod);

		if (unlikely(fod->aborted))
			nvmet_fc_abort_op(tgtport, fod->fcpreq);
		else
			nvmet_fc_xmt_fcp_rsp(tgtport, fod);

		break;

	case NVMET_FCOP_READDATA_RSP:
		if (fcpreq->fcp_error)
			/* overwrite the nvmet status */
			cqe->status = cpu_to_le16(fcpreq->fcp_error);

		else if (fcpreq->transferred_length !=
					fcpreq->transfer_length)
			/* overwrite the nvmet status */
			cqe->status = cpu_to_le16(NVME_SC_FC_TRANSPORT_ERROR);

		else
			fod->offset += fcpreq->transferred_length;

		/* data transfer complete, response complete as well */

		/* data no longer needed */
		nvmet_fc_free_tgt_pgs(fod);

		dma_sync_single_for_cpu(tgtport->dev, fod->rspdma,
				sizeof(fod->rspiubuf), DMA_TO_DEVICE);
		nvmet_fc_free_fcp_iod(tgtport, fod);
		break;

	case NVMET_FCOP_RSP:
	case NVMET_FCOP_ABORT:
		dma_sync_single_for_cpu(tgtport->dev, fod->rspdma,
				sizeof(fod->rspiubuf), DMA_TO_DEVICE);
		nvmet_fc_free_fcp_iod(tgtport, fod);
		break;

	default:
		fod->aborted = true;
		nvmet_fc_abort_op(tgtport, fod->fcpreq);
		break;
	}
}

static void
__nvmet_fc_fcp_nvme_cmd_done(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_fcp_iod *fod, int status)
{
	struct nvme_common_command *sqe = &fod->cmdiubuf.sqe.common;
	struct nvme_completion *cqe = &fod->rspiubuf.cqe;

	/* if an error handling the cmd post initial parsing */
	if (status) {
		/* fudge up a failed CQE status for our transport error */
		memset(cqe, 0, sizeof(*cqe));
		cqe->sq_head = fod->queue->sqhd;	/* echo last cqe sqhd */
		cqe->sq_id = cpu_to_le16(fod->queue->qid);
		cqe->command_id = sqe->command_id;
		cqe->status = cpu_to_le16(status);
	} else {
		/* snoop the last sq_head value from the last response */
		fod->queue->sqhd = cqe->sq_head;

		/*
		 * try to push the data even if the SQE status is non-zero.
		 * There may be a status where data still was intended to
		 * be moved
		 */
		if ((fod->io_dir == NVMET_FCP_READ) && (fod->data_sg_cnt)) {
			/* push the data over before sending rsp */
			nvmet_fc_transfer_fcp_data(tgtport, fod,
						NVMET_FCOP_READDATA);
			return;
		}

		/* writes & no data - fall thru */
	}

	/* data no longer needed */
	nvmet_fc_free_tgt_pgs(fod);

	nvmet_fc_xmt_fcp_rsp(tgtport, fod);
}


static void
nvmet_fc_fcp_nvme_cmd_done(struct nvmet_req *nvme_req)
{
	struct nvmet_fc_fcp_iod *fod =
		container_of(nvme_req, struct nvmet_fc_fcp_iod, req);
	struct nvmet_fc_tgtport *tgtport = fod->tgtport;

	__nvmet_fc_fcp_nvme_cmd_done(tgtport, fod, 0);
}


/*
 * Actual processing routine for received FC-NVME LS Requests from the LLD
 */
void
nvmet_fc_handle_fcp_rqst(struct nvmet_fc_tgtport *tgtport,
			struct nvmet_fc_fcp_iod *fod)
{
	struct nvme_fc_cmd_iu *cmdiu = &fod->cmdiubuf;
	int ret;

	/*
	 * TODO: handle fused cmds back-to-back
	 */

	fod->fcpreq->done = nvmet_fc_xmt_fcp_op_done;

	fod->total_length = be32_to_cpu(cmdiu->data_len);
	if (cmdiu->flags & FCNVME_CMD_FLAGS_WRITE) {
		fod->io_dir = NVMET_FCP_WRITE;
		if (!nvme_is_write(&cmdiu->sqe))
			goto transport_error;
	} else if (cmdiu->flags & FCNVME_CMD_FLAGS_READ) {
		fod->io_dir = NVMET_FCP_READ;
		if (nvme_is_write(&cmdiu->sqe))
			goto transport_error;
	} else {
		fod->io_dir = NVMET_FCP_NODATA;
		if (fod->total_length)
			goto transport_error;
	}
	fod->aborted = false;

	fod->req.cmd = &fod->cmdiubuf.sqe;
	fod->req.rsp = &fod->rspiubuf.cqe;
	fod->req.port = fod->queue->port;

	/* ensure nvmet handlers will set cmd handler callback */
	fod->req.execute = NULL;

	/* clear any response payload */
	memset(&fod->rspiubuf, 0, sizeof(fod->rspiubuf));

	ret = nvmet_req_init(&fod->req,
				&fod->queue->nvme_cq,
				&fod->queue->nvme_sq,
				&nvmet_fc_tgt_fcp_ops);
	if (!ret) {	/* bad SQE content */
		__nvmet_fc_fcp_nvme_cmd_done(tgtport, fod,
				NVME_SC_FC_TRANSPORT_ERROR);
		return;
	}

	/* keep a running counter of tail position */
	atomic_inc(&fod->queue->sqtail);

	fod->data_sg = NULL;
	fod->data_sg_cnt = 0;
	if (fod->total_length) {
		ret = nvmet_fc_alloc_tgt_pgs(fod);
		if (ret) {
			nvmet_req_complete(&fod->req, ret);
			return;
		}
	}
	fod->req.sg = fod->data_sg;
	fod->req.sg_cnt = fod->data_sg_cnt;
	fod->offset = 0;
	fod->next_sg = fod->data_sg;
	fod->next_sg_offset = 0;

	if (fod->io_dir == NVMET_FCP_WRITE) {
		/* pull the data over before invoking nvmet layer */
		nvmet_fc_transfer_fcp_data(tgtport, fod, NVMET_FCOP_WRITEDATA);
		return;
	}

	/*
	 * Reads or no data:
	 *
	 * can invoke the nvmet_layer now. If read data, cmd completion will
	 * push the data
	 */

	fod->req.execute(&fod->req);

	return;

transport_error:
	__nvmet_fc_fcp_nvme_cmd_done(tgtport, fod, NVME_SC_FC_TRANSPORT_ERROR);
}

/*
 * Actual processing routine for received FC-NVME LS Requests from the LLD
 */
static void
nvmet_fc_handle_fcp_rqst_work(struct work_struct *work)
{
	struct nvmet_fc_fcp_iod *fod =
		container_of(work, struct nvmet_fc_fcp_iod, work);
	struct nvmet_fc_tgtport *tgtport = fod->tgtport;

	nvmet_fc_handle_fcp_rqst(tgtport, fod);
}


/**
 * nvmet_fc_rcv_fcp_req - transport entry point called by an LLDD
 *                       upon the reception of a NVME FCP CMD IU.
 *
 * Pass a FC-NVME FCP CMD IU received from the FC link to the nvmet-fc
 * layer for processing.
 *
 * The nvmet-fc layer will copy cmd payload to an internal structure for
 * processing.  As such, upon completion of the routine, the LLDD may
 * immediately free/reuse the CMD IU buffer passed in the call.
 *
 * If this routine returns error, the lldd should abort the exchange.
 *
 * @tgtport:    pointer to the (registered) target port the FCP CMD IU
 *              was receive on.
 * @fcpreq:     pointer to a fcpreq request structure to be used to reference
 *              the exchange corresponding to the FCP Exchange.
 * @cmdiubuf:   pointer to the buffer containing the FCP CMD IU
 * @cmdiubuf_len: length, in bytes, of the received FCP CMD IU
 */
int
nvmet_fc_rcv_fcp_req(struct nvmet_fc_target_port *target_port,
			struct nvmefc_tgt_fcp_req *fcpreq,
			void *cmdiubuf, u32 cmdiubuf_len)
{
	struct nvmet_fc_tgtport *tgtport = container_of(target_port,
			struct nvmet_fc_tgtport, fc_target_port);
	struct nvme_fc_cmd_iu *cmdiu = cmdiubuf;
	struct nvmet_fc_tgt_queue *queue;
	struct nvmet_fc_fcp_iod *fod;


	/* validate iu, so the connection id can be used to find the queue */
	if ((cmdiubuf_len != sizeof(*cmdiu)) ||
			(cmdiu->scsi_id != NVME_CMD_SCSI_ID) ||
			(cmdiu->fc_id != NVME_CMD_FC_ID) ||
			(be16_to_cpu(cmdiu->iu_len) != (sizeof(*cmdiu)/4)))
		return -EIO;

	queue = nvmet_fc_find_target_queue(tgtport,
				be64_to_cpu(cmdiu->connection_id));
	if (!queue)
		return -ENOTCONN;

	fod = nvmet_fc_alloc_fcp_iod(tgtport, queue);
	if (!fod)
		return -ENOENT;

	fcpreq->nvmet_fc_private = fod;
	fod->fcpreq = fcpreq;
	memcpy(&fod->cmdiubuf, cmdiubuf, cmdiubuf_len);

	queue_work(fod->queue->work_q, &fod->work);

	return 0;
}
EXPORT_SYMBOL_GPL(nvmet_fc_rcv_fcp_req);

enum {
	FCT_TRADDR_ERR		= 0,
	FCT_TRADDR_FABRIC	= 1 << 0,
	FCT_TRADDR_WWNN		= 1 << 1,
	FCT_TRADDR_WWPN		= 1 << 2,
};

struct nvmet_fc_traddr {
	u64	fab;
	u64	nn;
	u64	pn;
};

static const match_table_t traddr_opt_tokens = {
	{ FCT_TRADDR_FABRIC,	"fab-%s"	},
	{ FCT_TRADDR_WWNN,	"nn-%s"		},
	{ FCT_TRADDR_WWPN,	"pn-%s"		},
	{ FCT_TRADDR_ERR,	NULL		}
};

static int
nvmet_fc_parse_traddr(struct nvmet_fc_traddr *traddr, char *buf)
{
	substring_t args[MAX_OPT_ARGS];
	char *options, *o, *p;
	int token, ret = 0;
	u64 token64;

	options = o = kstrdup(buf, GFP_KERNEL);
	if (!options)
		return -ENOMEM;

	while ((p = strsep(&o, ",\n")) != NULL) {
		if (!*p)
			continue;

		token = match_token(p, traddr_opt_tokens, args);
		switch (token) {
		case FCT_TRADDR_FABRIC:
			if (match_u64(args, &token64)) {
				ret = -EINVAL;
				goto out;
			}
			traddr->fab = token64;
			break;
		case FCT_TRADDR_WWNN:
			if (match_u64(args, &token64)) {
				ret = -EINVAL;
				goto out;
			}
			traddr->nn = token64;
			break;
		case FCT_TRADDR_WWPN:
			if (match_u64(args, &token64)) {
				ret = -EINVAL;
				goto out;
			}
			traddr->pn = token64;
			break;
		default:
			pr_warn("unknown traddr token or missing value '%s'\n",
					p);
			ret = -EINVAL;
			goto out;
		}
	}

out:
	kfree(options);
	return ret;
}

static int
nvmet_fc_add_port(struct nvmet_port *port)
{
	struct nvmet_fc_tgtport *tgtport;
	struct nvmet_fc_traddr traddr = { 0L, 0L, 0L };
	unsigned long flags;
	int ret;

	/* validate the address info */
	if ((port->disc_addr.trtype != NVMF_TRTYPE_FC) ||
	    (port->disc_addr.adrfam != NVMF_ADDR_FAMILY_FC))
		return -EINVAL;

	/* map the traddr address info to a target port */

	ret = nvmet_fc_parse_traddr(&traddr, port->disc_addr.traddr);
	if (ret)
		return ret;

	ret = -ENXIO;
	spin_lock_irqsave(&nvme_fc_tgtlock, flags);
	list_for_each_entry(tgtport, &nvmet_fc_target_list, tgt_list) {
		if ((tgtport->fc_target_port.node_name == traddr.nn) &&
		    (tgtport->fc_target_port.port_name == traddr.pn) &&
		    (tgtport->fc_target_port.fabric_name == traddr.fab)) {
			/* a FC port can only be 1 nvmet port id */
			if (tgtport->port)
				ret = -EALREADY;
			else {
				tgtport->port = port;
				port->priv = tgtport;
				ret = 0;
			}
			break;
		}
	}
	spin_unlock_irqrestore(&nvme_fc_tgtlock, flags);
	return ret;
}

static void
nvmet_fc_remove_port(struct nvmet_port *port)
{
	struct nvmet_fc_tgtport *tgtport = port->priv;
	unsigned long flags;

	spin_lock_irqsave(&nvme_fc_tgtlock, flags);
	if (tgtport->port == port)
		tgtport->port = NULL;
	spin_unlock_irqrestore(&nvme_fc_tgtlock, flags);
}

static struct nvmet_fabrics_ops nvmet_fc_tgt_fcp_ops = {
	.owner			= THIS_MODULE,
	.type			= NVMF_TRTYPE_FC,
	.msdbd			= 1,
	.add_port		= nvmet_fc_add_port,
	.remove_port		= nvmet_fc_remove_port,
	.queue_response		= nvmet_fc_fcp_nvme_cmd_done,
};

static int __init nvmet_fc_init_module(void)
{
	/* ensure NVMET_NR_QUEUES is a power of 2 - required for our masks */
	if (!is_power_of_2((unsigned long)NVMET_NR_QUEUES)) {
		pr_err("%s: NVMET_NR_QUEUES required to be power of 2\n",
				__func__);
		return(-EINVAL);
	}

	return nvmet_register_transport(&nvmet_fc_tgt_fcp_ops);
}

static void __exit nvmet_fc_exit_module(void)
{
	nvmet_unregister_transport(&nvmet_fc_tgt_fcp_ops);

	__nvmet_fc_free_tgtports();
}

module_init(nvmet_fc_init_module);
module_exit(nvmet_fc_exit_module);

MODULE_LICENSE("GPL v2");



