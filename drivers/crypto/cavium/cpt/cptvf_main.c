/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#include <linux/version.h>
#include <linux/aer.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/cpumask.h>

#include "cptvf.h"

#define DRV_NAME	"thunder-cptvf"
#define DRV_VERSION	"1.0"

static uint32_t qlen = DEFAULT_CMD_QLEN;
module_param(qlen, uint, 0644);
MODULE_PARM_DESC(qlen, "Command queue length");

static uint32_t chunksize = DEFAULT_CMD_QCHUNK_SIZE;
module_param(chunksize, uint, 0644);
MODULE_PARM_DESC(chunksize, "Command queue chunk size");

static uint32_t group = 1; /* Default to SE group */
module_param(group, uint, 0644);
MODULE_PARM_DESC(group, "VF group (Value between 0 - 7)");

static uint32_t priority;
module_param(priority, uint, 0644);
MODULE_PARM_DESC(priority, "VF/VQ Priority (0-1)");

struct cptvf_wqe {
	struct tasklet_struct twork;
	void *cptvf;
	uint32_t qno;
};

struct cptvf_wqe_info {
	struct cptvf_wqe vq_wqe[DEFAULT_DEVICE_QUEUES];
};

static void vq_work_handler(unsigned long data)
{
	struct cptvf_wqe_info *cwqe_info = (struct cptvf_wqe_info *)data;
	struct cptvf_wqe *cwqe = &cwqe_info->vq_wqe[0];

	vq_post_process(cwqe->cptvf, cwqe->qno);
}

static int init_worker_threads(struct cpt_vf *cptvf)
{
	struct pci_dev *pdev = cptvf->pdev;
	struct cptvf_wqe_info *cwqe_info;
	int i;

	cwqe_info = kzalloc(sizeof(*cwqe_info), GFP_KERNEL);
	if (!cwqe_info)
		return -ENOMEM;

	if (cptvf->nr_queues) {
		dev_info(&pdev->dev, "Creating VQ worker threads (%d)\n",
			 cptvf->nr_queues);
	}

	for (i = 0; i < cptvf->nr_queues; i++) {
		tasklet_init(&cwqe_info->vq_wqe[i].twork, vq_work_handler,
			     (uint64_t)cwqe_info);
		cwqe_info->vq_wqe[i].qno = i;
		cwqe_info->vq_wqe[i].cptvf = cptvf;
	}

	cptvf->wqe_info = cwqe_info;

	return 0;
}

static void cleanup_worker_threads(struct cpt_vf *cptvf)
{
	struct cptvf_wqe_info *cwqe_info;
	struct pci_dev *pdev = cptvf->pdev;
	int i;

	cwqe_info = (struct cptvf_wqe_info *)cptvf->wqe_info;
	if (!cwqe_info)
		return;

	if (cptvf->nr_queues) {
		dev_info(&pdev->dev, "Cleaning VQ worker threads (%u)\n",
			 cptvf->nr_queues);
	}

	for (i = 0; i < cptvf->nr_queues; i++)
		tasklet_kill(&cwqe_info->vq_wqe[i].twork);

	kzfree(cwqe_info);
	cptvf->wqe_info = NULL;
}

static void free_pending_queues(struct pending_qinfo *pqinfo)
{
	int32_t i;
	struct pending_queue *queue;

	for_each_pending_queue(pqinfo, queue, i) {
		if (!queue->head)
			continue;

		/* free single queue */
		kzfree((queue->head));

		queue->front = 0;
		queue->rear = 0;

		return;
	}

	pqinfo->qlen = 0;
	pqinfo->nr_queues = 0;
}

static int32_t alloc_pending_queues(struct pending_qinfo *pqinfo,
				    uint32_t qlen, uint32_t nr_queues)
{
	uint32_t i;
	size_t size;
	int32_t ret;
	struct pending_queue *queue = NULL;

	pqinfo->nr_queues = nr_queues;
	pqinfo->qlen = qlen;

	size = (qlen * sizeof(struct pending_entry));

	for_each_pending_queue(pqinfo, queue, i) {
		queue->head = kzalloc((size), GFP_KERNEL);
		if (!queue->head) {
			pr_err("pending Q (%d) allocation failed\n", i);
			ret = -ENOMEM;
			goto pending_qfail;
		}

		queue->front = 0;
		queue->rear = 0;
		atomic64_set((&queue->pending_count), (0));

		/* init queue spin lock */
		spin_lock_init(&queue->lock);
	}

	return 0;

pending_qfail:
	free_pending_queues(pqinfo);

	return ret;
}

static int32_t init_pending_queues(struct cpt_vf *cptvf, uint32_t qlen,
				   uint32_t nr_queues)
{
	int32_t ret;

	if (!nr_queues)
		return 0;

	ret = alloc_pending_queues(&cptvf->pqinfo, qlen, nr_queues);
	if (ret) {
		pr_err("failed to setup pending queues (%u)\n", nr_queues);
		return ret;
	}

	return 0;
}

static void cleanup_pending_queues(struct cpt_vf *cptvf)
{
	struct pci_dev *pdev = cptvf->pdev;

	if (!cptvf->nr_queues)
		return;

	dev_info(&pdev->dev, "Cleaning VQ pending queue (%u)\n",
		 cptvf->nr_queues);
	free_pending_queues(&cptvf->pqinfo);
}

static void free_command_queues(struct cpt_vf *cptvf,
				struct command_qinfo *cqinfo)
{
	int i, j;
	struct command_queue *queue = NULL;
	struct command_chunk *chunk = NULL, *next = NULL;
	struct pci_dev *pdev = cptvf->pdev;
	struct hlist_node *node;

	/* clean up for each queue */
	for (i = 0; i < cptvf->nr_queues; i++) {
		queue = &cqinfo->queue[i];
		if (hlist_empty(&cqinfo->queue[i].chead))
			continue;

		hlist_for_each(node, &cqinfo->queue[i].chead) {
			chunk = hlist_entry(node, struct command_chunk,
					    nextchunk);
			break;
		}

		for (j = 0; j < queue->nchunks; j++) {
			if (j < queue->nchunks) {
				node = node->next;
				next = hlist_entry(node, struct command_chunk,
						   nextchunk);
			}

			dma_free_coherent(&pdev->dev, chunk->size,
					  chunk->real_vaddr,
					  chunk->real_dma_addr);
			chunk->real_vaddr = NULL;
			chunk->real_dma_addr = 0;
			chunk->head = NULL;
			chunk->dma_addr = 0;
			hlist_del(&chunk->nextchunk);
			kzfree(chunk);
			chunk = next;
		}
		queue->nchunks = 0;
		queue->idx = 0;
		queue->dbell_count = 0;
	}

	/* common cleanup */
	cqinfo->cmd_size = 0;
	cqinfo->dbell_thold = 0;
}

static int32_t alloc_command_queues(struct cpt_vf *cptvf,
				    struct command_qinfo *cqinfo,
				    size_t cmd_size, size_t align,
				    uint32_t qlen, uint32_t nr_queues)
{
	int i;
	size_t q_size;
	struct command_queue *queue = NULL;
	struct pci_dev *pdev = cptvf->pdev;

	/* common init */
	cqinfo->cmd_size = cmd_size;
	cqinfo->dbell_thold = CPT_DBELL_THOLD;

	/* Qsize in dwords, needed for SADDR config, 1-next chunk pointer */
	cptvf->qsize = min(qlen, cqinfo->qchunksize) *
			CPT_NEXT_CHUNK_PTR_SIZE + 1;
	/* Qsize in bytes to create space for alignment */
	q_size = qlen * cqinfo->cmd_size;

	/* per queue initialization */
	for (i = 0; i < cptvf->nr_queues; i++) {
		size_t c_size = 0;
		size_t rem_q_size = q_size;
		struct command_chunk *curr = NULL, *first = NULL, *last = NULL;
		uint32_t qcsize_bytes = cqinfo->qchunksize * cqinfo->cmd_size;

		queue = &cqinfo->queue[i];
		INIT_HLIST_HEAD(&cqinfo->queue[i].chead);
		do {
			curr = kzalloc(sizeof(*curr), GFP_KERNEL);
			if (!curr)
				goto cmd_qfail;

			c_size = (rem_q_size > qcsize_bytes) ? qcsize_bytes :
					rem_q_size;
			curr->real_vaddr = (uint8_t *)dma_zalloc_coherent(&pdev->dev,
					  c_size + CPT_NEXT_CHUNK_PTR_SIZE,
					  &curr->real_dma_addr, GFP_KERNEL);
			if (!curr->real_vaddr) {
				pr_err("Command Q (%d) chunk (%d) allocation failed\n",
				       i, queue->nchunks);
				goto cmd_qfail;
			}

			curr->head = (uint8_t *)PTR_ALIGN(curr->real_vaddr, align);
			curr->dma_addr = (dma_addr_t)PTR_ALIGN(curr->real_dma_addr,
								align);
			curr->size = c_size;
			if (queue->nchunks == 0) {
				hlist_add_head(&curr->nextchunk,
					       &cqinfo->queue[i].chead);
				first = curr;
			} else {
				hlist_add_behind(&curr->nextchunk,
						 &last->nextchunk);
			}

			queue->nchunks++;
			rem_q_size -= c_size;
			if (last)
				*((uint64_t *)(&last->head[last->size])) = (uint64_t)curr->dma_addr;

			last = curr;
		} while (rem_q_size);

		/* Make the queue circular */
		/* Tie back last chunk entry to head */
		curr = first;
		*((uint64_t *)(&last->head[last->size])) = (uint64_t)curr->dma_addr;
		last->nextchunk.next = &curr->nextchunk;
		queue->qhead = curr;
		queue->dbell_count = 0;
		spin_lock_init(&queue->lock);
	}
	return 0;

cmd_qfail:
	free_command_queues(cptvf, cqinfo);
	return -ENOMEM;
}

static int32_t init_command_queues(struct cpt_vf *cptvf, uint32_t qlen,
				   uint32_t nr_queues)
{
	int32_t ret;

	if (!nr_queues)
		return 0;

	/* setup AE command queues */
	ret = alloc_command_queues(cptvf, &cptvf->cqinfo, CPT_INST_SIZE,
				   CPT_VQ_CHUNK_ALIGN, qlen, nr_queues);
	if (ret) {
		pr_err("failed to allocate AE command queues (%u)\n",
		       nr_queues);
		return ret;
	}

	return ret;
}

static void cleanup_command_queues(struct cpt_vf *cptvf)
{
	struct pci_dev *pdev = cptvf->pdev;

	if (!cptvf->nr_queues)
		return;

	dev_info(&pdev->dev, "Cleaning VQ command queue (%u)\n",
		 cptvf->nr_queues);
	free_command_queues(cptvf, &cptvf->cqinfo);
}

static void cptvf_sw_cleanup(struct cpt_vf *cptvf)
{
	cleanup_worker_threads(cptvf);
	cleanup_pending_queues(cptvf);
	cleanup_command_queues(cptvf);
}

static int32_t cptvf_sw_init(struct cpt_vf *cptvf, uint32_t qlen,
			     uint32_t nr_queues)
{
	int32_t ret = 0;
	uint32_t max_dev_queues = 0, nr_cpus = num_online_cpus();

	max_dev_queues = CPT_NUM_QS_PER_VF;
	/* possible cpus */
	nr_queues = max_t(uint32_t, nr_cpus, nr_queues);
	nr_queues = min_t(uint32_t, nr_queues, max_dev_queues);
	cptvf->max_queues = nr_queues;
	cptvf->nr_queues = nr_queues;
	cptvf->qlen = qlen;

	ret = init_command_queues(cptvf, qlen, nr_queues);
	if (ret) {
		pr_err("Failed to setup command queues (%u)\n", nr_queues);
		return ret;
	}

	ret = init_pending_queues(cptvf, qlen, nr_queues);
	if (ret) {
		pr_err("Failed to setup pending queues (%u)\n", nr_queues);
		goto setup_pqfail;
	}

	/* Create worker threads for BH processing */
	ret = init_worker_threads(cptvf);
	if (ret) {
		pr_err("Failed to setup worker threads\n");
		goto init_work_fail;
	}

	return 0;

init_work_fail:
	cleanup_worker_threads(cptvf);
	cleanup_pending_queues(cptvf);

setup_pqfail:
	cleanup_command_queues(cptvf);

	return ret;
}

static inline int cptvf_get_node_id(struct pci_dev *pdev)
{
	uint64_t addr = pci_resource_start(pdev, CPT_CSR_BAR);

	return ((addr >> CPT_NODE_ID_SHIFT) & CPT_NODE_ID_MASK);
}

static void cptvf_disable_msix(struct cpt_vf *cptvf)
{
	if (cptvf->msix_enabled) {
		pci_disable_msix(cptvf->pdev);
		cptvf->msix_enabled = 0;
		cptvf->num_vec = 0;
	}
}

static int cptvf_enable_msix(struct cpt_vf *cptvf)
{
	int i, ret;

	cptvf->num_vec = CPT_VF_MSIX_VECTORS;

	for (i = 0; i < cptvf->num_vec; i++)
		cptvf->msix_entries[i].entry = i;

	ret = pci_enable_msix(cptvf->pdev, cptvf->msix_entries,
			      cptvf->num_vec);
	if (ret) {
		dev_err(&cptvf->pdev->dev, "Request for #%d msix vectors failed\n",
			cptvf->num_vec);
		return ret;
	}

	cptvf->msix_enabled = 1;
	/* Mark MSIX enabled */
	cptvf->flags |= CPT_FLAG_MSIX_ENABLED;

	return 0;
}

static void cptvf_free_all_interrupts(struct cpt_vf *cptvf)
{
	int irq;

	for (irq = 0; irq < cptvf->num_vec; irq++) {
		if (cptvf->irq_allocated[irq])
			irq_set_affinity_hint(cptvf->msix_entries[irq].vector,
					      NULL);
		free_cpumask_var(cptvf->affinity_mask[irq]);
		free_irq(cptvf->msix_entries[irq].vector, cptvf);
		cptvf->irq_allocated[irq] = false;
	}
}

static void cptvf_write_vq_ctl(struct cpt_vf *cptvf, bool val)
{
	union cptx_vqx_ctl vqx_ctl;

	vqx_ctl.u = cpt_read_csr64(cptvf->reg_base, CPTX_VQX_CTL(0, 0));
	vqx_ctl.s.ena = val;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_CTL(0, 0), vqx_ctl.u);
}

void cptvf_write_vq_doorbell(struct cpt_vf *cptvf, uint32_t val)
{
	union cptx_vqx_doorbell vqx_dbell;

	vqx_dbell.u = cpt_read_csr64(cptvf->reg_base,
				     CPTX_VQX_DOORBELL(0, 0));
	vqx_dbell.s.dbell_cnt = val * 8; /* Num of Instructions * 8 words */
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_DOORBELL(0, 0),
			vqx_dbell.u);
}

static void cptvf_write_vq_inprog(struct cpt_vf *cptvf, uint8_t val)
{
	union cptx_vqx_inprog vqx_inprg;

	vqx_inprg.u = cpt_read_csr64(cptvf->reg_base, CPTX_VQX_INPROG(0, 0));
	vqx_inprg.s.inflight = val;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_INPROG(0, 0), vqx_inprg.u);
}

static void cptvf_write_vq_done_numwait(struct cpt_vf *cptvf, uint32_t val)
{
	union cptx_vqx_done_wait vqx_dwait;

	vqx_dwait.u = cpt_read_csr64(cptvf->reg_base,
				     CPTX_VQX_DONE_WAIT(0, 0));
	vqx_dwait.s.num_wait = val;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_DONE_WAIT(0, 0),
			vqx_dwait.u);
}

static void cptvf_write_vq_done_timewait(struct cpt_vf *cptvf, uint16_t val)
{
	union cptx_vqx_done_wait vqx_dwait;

	vqx_dwait.u = cpt_read_csr64(cptvf->reg_base,
				     CPTX_VQX_DONE_WAIT(0, 0));
	vqx_dwait.s.time_wait = val;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_DONE_WAIT(0, 0),
			vqx_dwait.u);
}

static void cptvf_enable_swerr_interrupts(struct cpt_vf *cptvf)
{
	union cptx_vqx_misc_ena_w1s vqx_misc_ena;

	vqx_misc_ena.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_MISC_ENA_W1S(0, 0));
	/* Set mbox(0) interupts for the requested vf */
	vqx_misc_ena.s.swerr = 1;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_MISC_ENA_W1S(0, 0),
			vqx_misc_ena.u);
}

static void cptvf_enable_mbox_interrupts(struct cpt_vf *cptvf)
{
	union cptx_vqx_misc_ena_w1s vqx_misc_ena;

	vqx_misc_ena.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_MISC_ENA_W1S(0, 0));
	/* Set mbox(0) interupts for the requested vf */
	vqx_misc_ena.s.mbox = 1;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_MISC_ENA_W1S(0, 0),
			vqx_misc_ena.u);
}

static void cptvf_enable_done_interrupts(struct cpt_vf *cptvf)
{
	union cptx_vqx_done_ena_w1s vqx_done_ena;

	vqx_done_ena.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_DONE_ENA_W1S(0, 0));
	/* Set DONE interrupt for the requested vf */
	vqx_done_ena.s.done = 1;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_DONE_ENA_W1S(0, 0),
			vqx_done_ena.u);
}

static void cptvf_clear_dovf_intr(struct cpt_vf *cptvf)
{
	union cptx_vqx_misc_int vqx_misc_int;

	vqx_misc_int.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_MISC_INT(0, 0));
	/* W1C for the VF */
	vqx_misc_int.s.dovf = 1;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_MISC_INT(0, 0),
			vqx_misc_int.u);
}

static void cptvf_clear_irde_intr(struct cpt_vf *cptvf)
{
	union cptx_vqx_misc_int vqx_misc_int;

	vqx_misc_int.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_MISC_INT(0, 0));
	/* W1C for the VF */
	vqx_misc_int.s.irde = 1;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_MISC_INT(0, 0),
			vqx_misc_int.u);
}

static void cptvf_clear_nwrp_intr(struct cpt_vf *cptvf)
{
	union cptx_vqx_misc_int vqx_misc_int;

	vqx_misc_int.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_MISC_INT(0, 0));
	/* W1C for the VF */
	vqx_misc_int.s.nwrp = 1;
	cpt_write_csr64(cptvf->reg_base,
			CPTX_VQX_MISC_INT(0, 0), vqx_misc_int.u);
}

static void cptvf_clear_mbox_intr(struct cpt_vf *cptvf)
{
	union cptx_vqx_misc_int vqx_misc_int;

	vqx_misc_int.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_MISC_INT(0, 0));
	/* W1C for the VF */
	vqx_misc_int.s.mbox = 1;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_MISC_INT(0, 0),
			vqx_misc_int.u);
}

static void cptvf_clear_swerr_intr(struct cpt_vf *cptvf)
{
	union cptx_vqx_misc_int vqx_misc_int;

	vqx_misc_int.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_MISC_INT(0, 0));
	/* W1C for the VF */
	vqx_misc_int.s.swerr = 1;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_MISC_INT(0, 0),
			vqx_misc_int.u);
}

static uint64_t cptvf_read_vf_misc_intr_status(struct cpt_vf *cptvf)
{
	return cpt_read_csr64(cptvf->reg_base, CPTX_VQX_MISC_INT(0, 0));
}

static irqreturn_t cptvf_misc_intr_handler(int irq, void *cptvf_irq)
{
	struct cpt_vf *cptvf = (struct cpt_vf *)cptvf_irq;
	uint64_t intr;

	intr = cptvf_read_vf_misc_intr_status(cptvf);
	/*Check for MISC interrupt types*/
	if (likely(intr & CPT_VF_INTR_MBOX_MASK)) {
		pr_err("Mailbox interrupt 0x%llx on CPT VF %d\n",
		       intr, cptvf->vfid);
		cptvf_handle_mbox_intr(cptvf);
		cptvf_clear_mbox_intr(cptvf);
	} else if (unlikely(intr & CPT_VF_INTR_DOVF_MASK)) {
		cptvf_clear_dovf_intr(cptvf);
		/*Clear doorbell count*/
		cptvf_write_vq_doorbell(cptvf, 0);
		pr_err("Doorbell overflow error interrupt 0x%llx on CPT VF %d\n",
		       intr, cptvf->vfid);
	} else if (unlikely(intr & CPT_VF_INTR_IRDE_MASK)) {
		cptvf_clear_irde_intr(cptvf);
		pr_err("Instruction NCB read error interrupt 0x%llx on CPT VF %d\n",
		       intr, cptvf->vfid);
	} else if (unlikely(intr & CPT_VF_INTR_NWRP_MASK)) {
		cptvf_clear_nwrp_intr(cptvf);
		pr_err("NCB response write error interrupt 0x%llx on CPT VF %d\n",
		       intr, cptvf->vfid);
	} else if (unlikely(intr & CPT_VF_INTR_SERR_MASK)) {
		cptvf_clear_swerr_intr(cptvf);
		pr_err("Software error interrupt 0x%llx on CPT VF %d\n",
		       intr, cptvf->vfid);
	} else {
		pr_err("Unhandled interrupt in CPT VF %d\n", cptvf->vfid);
	}

	return IRQ_HANDLED;
}

static inline struct cptvf_wqe *get_cptvf_vq_wqe(struct cpt_vf *cptvf,
						 int qno)
{
	struct cptvf_wqe_info *nwqe_info;

	if (unlikely(qno >= cptvf->nr_queues))
		return NULL;
	nwqe_info = (struct cptvf_wqe_info *)cptvf->wqe_info;

	return &nwqe_info->vq_wqe[qno];
}

static inline uint32_t cptvf_read_vq_done_count(struct cpt_vf *cptvf)
{
	union cptx_vqx_done vqx_done;

	vqx_done.u = cpt_read_csr64(cptvf->reg_base, CPTX_VQX_DONE(0, 0));
	return vqx_done.s.done;
}

static inline void cptvf_write_vq_done_ack(struct cpt_vf *cptvf,
					   uint32_t ackcnt)
{
	union cptx_vqx_done_ack vqx_dack_cnt;

	vqx_dack_cnt.u = cpt_read_csr64(cptvf->reg_base,
					CPTX_VQX_DONE_ACK(0, 0));
	vqx_dack_cnt.s.done_ack = ackcnt;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_DONE_ACK(0, 0),
			vqx_dack_cnt.u);
}

static irqreturn_t cptvf_done_intr_handler(int irq, void *cptvf_irq)
{
	struct cpt_vf *cptvf = (struct cpt_vf *)cptvf_irq;
	/* Read the number of completions */
	uint32_t intr = cptvf_read_vq_done_count(cptvf);

	cptvf->intcnt += intr;
	if (intr) {
		struct cptvf_wqe *wqe;

		/* Acknowledge the number of
		 * scheduled completions for processing
		 */
		cptvf_write_vq_done_ack(cptvf, intr);
		wqe = get_cptvf_vq_wqe(cptvf, 0);
		if (unlikely(!wqe)) {
			pr_err("No work to schedule for VF (%d)",
			       cptvf->vfid);
			return 1;
		}
		tasklet_hi_schedule(&wqe->twork);
	}

	return IRQ_HANDLED;
}

static int cptvf_register_misc_intr(struct cpt_vf *cptvf)
{
	int ret;
	struct device *dev = &cptvf->pdev->dev;

	/* Register misc interrupt handlers */
	ret = request_irq(cptvf->msix_entries[CPT_VF_INT_VEC_E_MISC].vector,
			  cptvf_misc_intr_handler, 0, "CPT VF misc intr",
			  cptvf);
	if (ret)
		goto fail;

	cptvf->irq_allocated[CPT_VF_INT_VEC_E_MISC] = true;

	/* Enable mailbox interrupt */
	cptvf_enable_mbox_interrupts(cptvf);
	cptvf_enable_swerr_interrupts(cptvf);

	return 0;

fail:
	dev_err(dev, "Request misc irq failed");
	cptvf_free_all_interrupts(cptvf);
	return ret;
}

static int cptvf_register_done_intr(struct cpt_vf *cptvf)
{
	int ret;
	struct device *dev = &cptvf->pdev->dev;

	/* Register DONE interrupt handlers */
	ret = request_irq(cptvf->msix_entries[CPT_VF_INT_VEC_E_DONE].vector,
			  cptvf_done_intr_handler, 0, "CPT VF done intr",
			  cptvf);
	if (ret)
		goto fail;

	cptvf->irq_allocated[CPT_VF_INT_VEC_E_DONE] = true;

	/* Enable mailbox interrupt */
	cptvf_enable_done_interrupts(cptvf);
	return 0;

fail:
	dev_err(dev, "Request done irq failed\n");
	cptvf_free_all_interrupts(cptvf);
	return ret;
}

static void cptvf_unregister_interrupts(struct cpt_vf *cptvf)
{
	cptvf_free_all_interrupts(cptvf);
	cptvf_disable_msix(cptvf);
}

static void cptvf_set_irq_affinity(struct cpt_vf *cptvf)
{
	int32_t vec, cpu;
	int32_t irqnum;

	for (vec = 0; vec < cptvf->num_vec; vec++) {
		if (!cptvf->irq_allocated[vec])
			continue;

		if (!zalloc_cpumask_var(&cptvf->affinity_mask[vec],
					GFP_KERNEL)) {
			pr_err("Allocation failed for affinity_mask for VF %d",
			       cptvf->vfid);
			return;
		}

		cpu = cptvf->vfid % num_online_cpus();
		cpumask_set_cpu(cpumask_local_spread(cpu, cptvf->node),
				cptvf->affinity_mask[vec]);
		irqnum = cptvf->msix_entries[vec].vector;
		irq_set_affinity_hint(irqnum, cptvf->affinity_mask[vec]);
	}
}

static void cptvf_write_vq_saddr(struct cpt_vf *cptvf, uint64_t val)
{
	union cptx_vqx_saddr vqx_saddr;

	vqx_saddr.u = val;
	cpt_write_csr64(cptvf->reg_base, CPTX_VQX_SADDR(0, 0), vqx_saddr.u);
}

void cptvf_device_init(struct cpt_vf *cptvf)
{
	uint64_t base_addr = 0;

	cptvf->chip_id = CPTVF_81XX_PASS1_0;
	/* Disable the VQ */
	cptvf_write_vq_ctl(cptvf, 0);
	/* Reset the doorbell */
	cptvf_write_vq_doorbell(cptvf, 0);
	/* Clear inflight */
	cptvf_write_vq_inprog(cptvf, 0);
	/* Write VQ SADDR */
	/* TODO: for now only one queue, so hard coded */
	base_addr = (uint64_t)(cptvf->cqinfo.queue[0].qhead->dma_addr);
	cptvf_write_vq_saddr(cptvf, base_addr);
	/* Configure timerhold / coalescence */
	cptvf_write_vq_done_timewait(cptvf, CPT_TIMER_THOLD);
	cptvf_write_vq_done_numwait(cptvf, CPT_COUNT_THOLD);
	/* Enable the VQ */
	cptvf_write_vq_ctl(cptvf, 1);
	/* Flag the VF ready */
	cptvf->flags |= CPT_FLAG_DEVICE_READY;
}

static int cptvf_probe(struct pci_dev *pdev, const struct pci_device_id *ent)
{
	struct device *dev = &pdev->dev;
	struct cpt_vf *cptvf;
	int    err;

	cptvf = devm_kzalloc(dev, sizeof(struct cpt_vf), GFP_KERNEL);
	if (!cptvf)
		return -ENOMEM;

	pci_set_drvdata(pdev, cptvf);
	cptvf->pdev = pdev;
	err = pci_enable_device(pdev);
	if (err) {
		dev_err(dev, "Failed to enable PCI device\n");
		pci_set_drvdata(pdev, NULL);
		return err;
	}

	err = pci_request_regions(pdev, DRV_NAME);
	if (err) {
		dev_err(dev, "PCI request regions failed 0x%x\n", err);
		goto cptvf_err_disable_device;
	}
	/* Mark as VF driver */
	cptvf->flags |= CPT_FLAG_VF_DRIVER;
	err = pci_set_dma_mask(pdev, DMA_BIT_MASK(48));
	if (err) {
		dev_err(dev, "Unable to get usable DMA configuration\n");
		goto cptvf_err_release_regions;
	}

	err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(48));
	if (err) {
		dev_err(dev, "Unable to get 48-bit DMA for consistent allocations\n");
		goto cptvf_err_release_regions;
	}

	/* MAP PF's configuration registers */
	cptvf->reg_base = pcim_iomap(pdev, CPT_CSR_BAR, 0);
	if (!cptvf->reg_base) {
		dev_err(dev, "Cannot map config register space, aborting\n");
		err = -ENOMEM;
		goto cptvf_err_release_regions;
	}

	cptvf->node = cptvf_get_node_id(pdev);
	/* Enable MSI-X */
	err = cptvf_enable_msix(cptvf);
	if (err) {
		dev_err(dev, "cptvf_enable_msix() failed");
		goto cptvf_err_release_regions;
	}

	/* Register mailbox interrupts */
	cptvf_register_misc_intr(cptvf);

	/* Check ready with PF */
	/* Gets chip ID / device Id from PF if ready */
	err = cptvf_check_pf_ready(cptvf);
	if (err) {
		dev_err(dev, "PF not responding to READY msg");
		err = -EBUSY;
		goto cptvf_err_release_regions;
	}

	/* CPT VF software resources initialization */
	cptvf->cqinfo.qchunksize = chunksize;
	err = cptvf_sw_init(cptvf, qlen, CPT_NUM_QS_PER_VF);
	if (err) {
		dev_err(dev, "cptvf_sw_init() failed");
		goto cptvf_err_release_regions;
	}
	/* Convey VQ LEN to PF */
	err = cptvf_send_vq_size_msg(cptvf);
	if (err) {
		dev_err(dev, "PF not responding to QLEN msg");
		err = -EBUSY;
		goto cptvf_err_release_regions;
	}

	/* CPT VF device initialization */
	cptvf_device_init(cptvf);
	/* Send msg to PF to assign currnet Q to required group */
	cptvf->vfgrp = group;
	err = cptvf_send_vf_to_grp_msg(cptvf);
	if (err) {
		dev_err(dev, "PF not responding to VF_GRP msg");
		err = -EBUSY;
		goto cptvf_err_release_regions;
	}

	cptvf->priority = priority;
	err = cptvf_send_vf_priority_msg(cptvf);
	if (err) {
		dev_err(dev, "PF not responding to VF_PRIO msg");
		err = -EBUSY;
		goto cptvf_err_release_regions;
	}
	/* Register DONE interrupts */
	err = cptvf_register_done_intr(cptvf);
	if (err)
		goto cptvf_err_release_regions;

	/* Set irq affinity masks */
	cptvf_set_irq_affinity(cptvf);
	/* Convey UP to PF */
	err = cptvf_send_vf_up(cptvf);
	if (err) {
		dev_err(dev, "PF not responding to UP msg");
		err = -EBUSY;
		goto cptvf_up_fail;
	}
	err = cvm_crypto_init(cptvf);
	if (err) {
		dev_err(dev, "Algorithm register failed\n");
		err = -EBUSY;
		goto cptvf_up_fail;
	}
	return 0;

cptvf_up_fail:
	cptvf_unregister_interrupts(cptvf);
cptvf_err_release_regions:
	pci_release_regions(pdev);
cptvf_err_disable_device:
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);

	return err;
}

static void cptvf_remove(struct pci_dev *pdev)
{
	struct cpt_vf *cptvf = pci_get_drvdata(pdev);

	if (!cptvf)
		pr_err("Invalid CPT-VF device\n");

	/* Convey DOWN to PF */
	if (cptvf_send_vf_down(cptvf)) {
		pr_err("PF not responding to DOWN msg");
	} else {
		cptvf_unregister_interrupts(cptvf);
		cptvf_sw_cleanup(cptvf);
		pci_set_drvdata(pdev, NULL);
		pci_release_regions(pdev);
		pci_disable_device(pdev);
		cvm_crypto_exit();
	}
}

static void cptvf_shutdown(struct pci_dev *pdev)
{
	cptvf_remove(pdev);
}

/* Supported devices */
static const struct pci_device_id cptvf_id_table[] = {
	{PCI_VDEVICE(CAVIUM, CPT_81XX_PCI_VF_DEVICE_ID), 0},
	{ 0, }  /* end of table */
};

static struct pci_driver cptvf_pci_driver = {
	.name = DRV_NAME,
	.id_table = cptvf_id_table,
	.probe = cptvf_probe,
	.remove = cptvf_remove,
	.shutdown = cptvf_shutdown,
};

static int __init cptvf_init_module(void)
{
	int ret = -1;

	pr_info("%s, ver %s\n", DRV_NAME, DRV_VERSION);
	if (group < 0 || group > 7) {
		pr_warn("Invalid group. Should be (0-7), setting to default 1.\n");
		group = 1;
	}

	if (chunksize > CPT_INST_CHUNK_MAX_SIZE || chunksize <= 0) {
		pr_warn("Invalid instruction chunk size. Should be (1-1023). Setting to default 1023\n");
		chunksize = CPT_INST_CHUNK_MAX_SIZE;
	}

	if ((qlen > chunksize) && (qlen % chunksize != 0)) {
		pr_warn("qlen should be multiple of chunksize when qlen > chunksize, rounding up qlen\n");
		qlen += chunksize - (qlen % chunksize);
	}

	if (priority < 0 || priority > 1) {
		pr_warn("Invalid VQ/VF priority. Should be (0-1), setting to default 0.\n");
		priority = 0;
	}

	ret = pci_register_driver(&cptvf_pci_driver);
	if (ret)
		pr_err("pci_register_driver() failed");

	return ret;
}

static void __exit cptvf_cleanup_module(void)
{
	pci_unregister_driver(&cptvf_pci_driver);
}

module_init(cptvf_init_module);
module_exit(cptvf_cleanup_module);

MODULE_AUTHOR("George Cherian <george.cherian@cavium.com>, Murthy Nidadavolu");
MODULE_DESCRIPTION("Cavium Thunder CPT Physical Function Driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRV_VERSION);
MODULE_DEVICE_TABLE(pci, cptvf_id_table);
