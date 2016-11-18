/*
 * Copyright (C) 2016 Cavium, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License
 * as published by the Free Software Foundation.
 */

#include <linux/bitmap.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/poll.h>

#include "cptvf.h"
#include "request_manager.h"

/**
 * get_free_pending_entry - get free entry from pending queue
 * @param pqinfo: pending_qinfo structure
 * @param qno: queue number
 */
static struct pending_entry *get_free_pending_entry(struct pending_queue *q,
						    int32_t qlen)
{
	struct pending_entry *ent = NULL;

	ent = &q->head[q->rear];
	if (unlikely(ent->busy)) {
		ent = NULL;
		goto no_free_entry;
	}

	q->rear++;
	if (unlikely(q->rear == qlen))
		q->rear = 0;

no_free_entry:
	return ent;
}

static inline void pending_queue_inc_front(struct pending_qinfo *pqinfo,
					   int32_t qno)
{
	struct pending_queue *queue = &pqinfo->queue[qno];

	queue->front++;
	if (unlikely(queue->front == pqinfo->qlen))
		queue->front = 0;
}

static int32_t setup_sgio_components(struct cpt_vf *cptvf,
				     struct buf_ptr *list,
				     int32_t buf_count, uint8_t *buffer)
{
	int32_t ret = 0, i, j;
	int32_t components;
	struct sglist_component *sg_ptr = NULL;
	struct pci_dev *pdev = cptvf->pdev;

	if (unlikely(!list)) {
		pr_err("Input List pointer is NULL\n");
		ret = -EFAULT;
		return ret;
	}

	for (i = 0; i < buf_count; i++) {
		if (likely(list[i].vptr)) {
			list[i].dma_addr = dma_map_single(&pdev->dev,
							  list[i].vptr,
							  list[i].size,
							  DMA_BIDIRECTIONAL);
			if (unlikely(dma_mapping_error(&pdev->dev,
						       list[i].dma_addr))) {
				pr_err("DMA map kernel buffer failed for component: %d\n",
				       i);
				ret = -EIO;
				goto sg_cleanup;
			}
		}
	}

	components = buf_count / 4;
	sg_ptr = (struct sglist_component *)buffer;
	for (i = 0; i < components; i++) {
		sg_ptr->u.s.len0 = cpu_to_be16(list[i * 4 + 0].size);
		sg_ptr->u.s.len1 = cpu_to_be16(list[i * 4 + 1].size);
		sg_ptr->u.s.len2 = cpu_to_be16(list[i * 4 + 2].size);
		sg_ptr->u.s.len3 = cpu_to_be16(list[i * 4 + 3].size);
		sg_ptr->ptr0 = cpu_to_be64(list[i * 4 + 0].dma_addr);
		sg_ptr->ptr1 = cpu_to_be64(list[i * 4 + 1].dma_addr);
		sg_ptr->ptr2 = cpu_to_be64(list[i * 4 + 2].dma_addr);
		sg_ptr->ptr3 = cpu_to_be64(list[i * 4 + 3].dma_addr);
		sg_ptr++;
	}

	components = buf_count % 4;

	switch (components) {
	case 3:
		sg_ptr->u.s.len2 = cpu_to_be16(list[i * 4 + 2].size);
		sg_ptr->ptr2 = cpu_to_be64(list[i * 4 + 2].dma_addr);
		/* Fall through */
	case 2:
		sg_ptr->u.s.len1 = cpu_to_be16(list[i * 4 + 1].size);
		sg_ptr->ptr1 = cpu_to_be64(list[i * 4 + 1].dma_addr);
		/* Fall through */
	case 1:
		sg_ptr->u.s.len0 = cpu_to_be16(list[i * 4 + 0].size);
		sg_ptr->ptr0 = cpu_to_be64(list[i * 4 + 0].dma_addr);
		break;
	default:
		break;
	}

	return ret;

sg_cleanup:
	for (j = 0; j < i; j++) {
		if (list[j].dma_addr) {
			dma_unmap_single(&pdev->dev, list[i].dma_addr,
					 list[i].size, DMA_BIDIRECTIONAL);
		}

		list[j].dma_addr = 0;
	}

	return ret;
}

static inline int32_t setup_sgio_list(struct cpt_vf *cptvf,
				      struct cpt_info_buffer *info,
				      struct cpt_request_info *req)
{
	uint16_t g_size_bytes = 0, s_size_bytes = 0;
	int32_t i = 0, ret = 0;
	struct pci_dev *pdev = cptvf->pdev;

	if ((req->incnt + req->outcnt) > MAX_SG_IN_OUT_CNT) {
		pr_err("Requestes SG components are higher than supported\n");
		ret = -EINVAL;
		goto  scatter_gather_clean;
	}

	/* Setup gather (input) components */
	info->g_size = (req->incnt + 3) / 4;
	info->glist_cnt = req->incnt;
	g_size_bytes = info->g_size * sizeof(struct sglist_component);
	for (i = 0; i < req->incnt; i++) {
		info->glist_ptr[i].vptr = req->in[i].ptr.addr;
		info->glist_ptr[i].size = req->in[i].size;
	}

	info->gather_components = kzalloc((g_size_bytes), GFP_KERNEL);
	if (!info->gather_components) {
		ret = -ENOMEM;
		goto  scatter_gather_clean;
	}

	ret = setup_sgio_components(cptvf, info->glist_ptr,
				    info->glist_cnt,
				    info->gather_components);
	if (ret) {
		pr_err("Failed to setup gather list\n");
		ret = -EFAULT;
		goto  scatter_gather_clean;
	}

	/* Setup scatter (output) components */
	info->s_size = (req->outcnt + 3) / 4;
	info->slist_cnt = req->outcnt;
	s_size_bytes = info->s_size * sizeof(struct sglist_component);
	for (i = 0; i < info->slist_cnt ; i++) {
		info->slist_ptr[i].vptr = req->out[i].ptr.addr;
		info->slist_ptr[i].size = req->out[i].size;
		info->outptr[i] = req->out[i].ptr.addr;
		info->outsize[i] = req->out[i].size;
		info->total_out += info->outsize[i];
	}

	info->scatter_components = kzalloc((s_size_bytes), GFP_KERNEL);
	if (!info->scatter_components) {
		ret = -ENOMEM;
		goto  scatter_gather_clean;
	}

	ret = setup_sgio_components(cptvf, info->slist_ptr,
				    info->slist_cnt,
				    info->scatter_components);
	if (ret) {
		pr_err("Failed to setup gather list\n");
		ret = -EFAULT;
		goto  scatter_gather_clean;
	}

	/* Create and initialize DPTR */
	info->dlen = g_size_bytes + s_size_bytes + SG_LIST_HDR_SIZE;
	info->in_buffer = kzalloc((info->dlen), GFP_KERNEL);
	if (!info->in_buffer) {
		ret = -ENOMEM;
		goto  scatter_gather_clean;
	}

	((uint16_t *)info->in_buffer)[0] = info->slist_cnt;
	((uint16_t *)info->in_buffer)[1] = info->glist_cnt;
	((uint16_t *)info->in_buffer)[2] = 0;
	((uint16_t *)info->in_buffer)[3] = 0;
	byte_swap_64((uint64_t *)info->in_buffer);

	memcpy(&info->in_buffer[8], info->gather_components,
	       g_size_bytes);
	memcpy(&info->in_buffer[8 + g_size_bytes],
	       info->scatter_components, s_size_bytes);

	info->dptr_baddr = dma_map_single(&pdev->dev,
					       (void *)info->in_buffer,
					       info->dlen,
					       DMA_BIDIRECTIONAL);
	if (dma_mapping_error(&pdev->dev, info->dptr_baddr)) {
		pr_err("Mapping DPTR Failed %d\n", info->dlen);
		ret = -EIO;
		goto  scatter_gather_clean;
	}

	/* Create and initialize RPTR */
	info->rlen = COMPLETION_CODE_SIZE;
	info->out_buffer = kzalloc((info->rlen), GFP_KERNEL);
	if (!info->out_buffer) {
		ret = -ENOMEM;
		goto  scatter_gather_clean;
	}

	*((uint64_t *)info->out_buffer) = ~((uint64_t)COMPLETION_CODE_INIT);
	info->alternate_caddr = (uint64_t *)info->out_buffer;
	info->rptr_baddr = dma_map_single(&pdev->dev,
					       (void *)info->out_buffer,
					       info->rlen,
					       DMA_BIDIRECTIONAL);
	if (dma_mapping_error(&pdev->dev, info->rptr_baddr)) {
		pr_err("Mapping RPTR Failed %d\n", info->rlen);
		ret = -EIO;
		goto  scatter_gather_clean;
	}

	return 0;

scatter_gather_clean:
	return ret;
}

int32_t send_cpt_command(struct cpt_vf *cptvf, union cpt_inst_s *cmd,
			 uint32_t qno)
{
	struct command_qinfo *qinfo = NULL;
	struct command_queue *queue;
	struct command_chunk *chunk;
	uint8_t *ent;
	int32_t ret = 0;

	if (unlikely(qno >= cptvf->nr_queues)) {
		pr_err("Invalid queue (qno: %d, nr_queues: %d)\n",
		       qno, cptvf->nr_queues);
		return -EINVAL;
	}

	qinfo = &cptvf->cqinfo;
	queue = &qinfo->queue[qno];
	/* lock commad queue */
	spin_lock(&queue->lock);
	ent = &queue->qhead->head[queue->idx * qinfo->cmd_size];
	memcpy(ent, (void *)cmd, qinfo->cmd_size);

	if (++queue->idx >= queue->qhead->size / 64) {
		struct hlist_node *node;

		hlist_for_each(node, &queue->chead) {
			chunk = hlist_entry(node, struct command_chunk,
					    nextchunk);
			if (chunk == queue->qhead) {
				continue;
			} else {
				queue->qhead = chunk;
				break;
			}
		}
		queue->idx = 0;
	}
	/* make sure all memory stores are done before ringing doorbell */
	smp_wmb();
	cptvf_write_vq_doorbell(cptvf, 1);
	/* unlock command queue */
	spin_unlock(&queue->lock);

	return ret;
}

void do_request_cleanup(struct cpt_vf *cptvf,
			struct cpt_info_buffer *info)
{
	int32_t i;
	struct pci_dev *pdev = cptvf->pdev;

	if (info->dptr_baddr) {
		dma_unmap_single(&pdev->dev, info->dptr_baddr,
				 info->dlen, DMA_BIDIRECTIONAL);
		info->dptr_baddr = 0;
	}

	if (info->rptr_baddr) {
		dma_unmap_single(&pdev->dev, info->rptr_baddr,
				 info->rlen, DMA_BIDIRECTIONAL);
		info->rptr_baddr = 0;
	}

	if (info->comp_baddr) {
		dma_unmap_single(&pdev->dev, info->comp_baddr,
				 sizeof(union cpt_res_s), DMA_BIDIRECTIONAL);
		info->comp_baddr = 0;
	}

	if (info->dma_mode == DMA_GATHER_SCATTER) {
		for (i = 0; i < info->slist_cnt; i++) {
			if (info->slist_ptr[i].dma_addr) {
				dma_unmap_single(&pdev->dev,
						 info->slist_ptr[i].dma_addr,
						 info->slist_ptr[i].size,
						 DMA_BIDIRECTIONAL);
				info->slist_ptr[i].dma_addr = 0ULL;
			}
		}
		info->slist_cnt = 0;
		if (info->scatter_components)
			kzfree(info->scatter_components);

		for (i = 0; i < info->glist_cnt; i++) {
			if (info->glist_ptr[i].dma_addr) {
				dma_unmap_single(&pdev->dev,
						 info->glist_ptr[i].dma_addr,
						 info->glist_ptr[i].size,
						 DMA_BIDIRECTIONAL);
				info->glist_ptr[i].dma_addr = 0ULL;
			}
		}
		info->glist_cnt = 0;
		if (info->gather_components)
			kzfree((info->gather_components));
	}

	if (info->out_buffer) {
		kzfree((info->out_buffer));
		info->out_buffer = NULL;
	}

	if (info->in_buffer) {
		kzfree((info->in_buffer));
		info->in_buffer = NULL;
	}

	if (info->completion_addr) {
		kzfree(((void *)info->completion_addr));
		info->completion_addr = NULL;
	}

	if (info) {
		kzfree((info));
		info = NULL;
	}
}

void do_post_process(struct cpt_vf *cptvf, struct cpt_info_buffer *info)
{
	uint64_t *p;
	uint32_t i;

	if (!info || !cptvf) {
		pr_err("Input params are incorrect for post processing\n");
		return;
	}

	if (info->rlen) {
		for (i = 0; i < info->slist_cnt; i++) {
			if (info->outunit[i] == UNIT_64_BIT) {
				p = (uint64_t *)info->slist_ptr[i].vptr;
				*p = cpu_to_be64(*p);
			}
		}
	}

	do_request_cleanup(cptvf, info);
}

static inline void process_pending_queue(struct cpt_vf *cptvf,
					 struct pending_qinfo *pqinfo,
					 int32_t qno)
{
	struct pending_queue *pqueue = &pqinfo->queue[qno];
	struct pending_entry *pentry = NULL;
	struct cpt_info_buffer *info = NULL;
	union cpt_res_s *status = NULL;

	while (1) {
		spin_lock_bh(&pqueue->lock);
		pentry = &pqueue->head[pqueue->front];
		if (unlikely(!pentry->busy)) {
			spin_unlock_bh(&pqueue->lock);
			break;
		}

		info = (struct cpt_info_buffer *)pentry->post_arg;
		if (unlikely(!info)) {
			pr_err("Pending Entry post arg NULL\n");
			pending_queue_inc_front(pqinfo, qno);
			spin_unlock_bh(&pqueue->lock);
			continue;
		}

		status = (union cpt_res_s *)pentry->completion_addr;
		if ((status->s.compcode == CPT_COMP_E_FAULT) ||
		    (status->s.compcode == CPT_COMP_E_SWERR)) {
			pr_err("Request failed with %s\n",
			       (status->s.compcode == CPT_COMP_E_FAULT) ?
			       "DMA Fault" : "Software error");
			pentry->completion_addr = NULL;
			pentry->busy = false;
			atomic64_dec((&pqueue->pending_count));
			pentry->post_arg = NULL;
			pending_queue_inc_front(pqinfo, qno);
			do_request_cleanup(cptvf, info);
			spin_unlock_bh(&pqueue->lock);
			break;
		} else if (status->s.compcode == COMPLETION_CODE_INIT) {
			/* check for timeout */
			if (time_after_eq(jiffies,
			    (info->time_in + (DEFAULT_COMMAND_TIMEOUT * HZ)))) {
				pr_err("Request timed out");
				pentry->completion_addr = NULL;
				pentry->busy = false;
				atomic64_dec((&pqueue->pending_count));
				pentry->post_arg = NULL;
				pending_queue_inc_front(pqinfo, qno);
				do_request_cleanup(cptvf, info);
				spin_unlock_bh(&pqueue->lock);
				break;
			} else if ((*info->alternate_caddr ==
				(~COMPLETION_CODE_INIT)) &&
				(info->extra_time < TIME_IN_RESET_COUNT)) {
				info->time_in = jiffies;
				info->extra_time++;
				spin_unlock_bh(&pqueue->lock);
				break;
			}
		}

		info->status = 0;
		pentry->completion_addr = NULL;
		pentry->busy = false;
		pentry->post_arg = NULL;
		atomic64_dec((&pqueue->pending_count));
		pending_queue_inc_front(pqinfo, qno);
		spin_unlock_bh(&pqueue->lock);

		do_post_process(info->cptvf, info);
		/*
		 * Calling callback after we find
		 * that the request has been serviced
		 */
		pentry->callback(status->s.compcode, pentry->callback_arg);
	}
}

int32_t process_request(struct cpt_vf *cptvf, struct cpt_request_info *req)
{
	int32_t ret = 0, clear = 0, queue = 0;
	struct cpt_info_buffer *info = NULL;
	struct cptvf_request *cpt_req = NULL;
	union ctrl_info *ctrl = NULL;
	struct pending_entry *pentry = NULL;
	struct pending_queue *pqueue = NULL;
	struct pci_dev *pdev = cptvf->pdev;
	uint64_t key_handle = 0ULL;
	uint8_t group = 0;
	struct cpt_vq_command vq_cmd;
	union cpt_inst_s cptinst;

	if (unlikely(!cptvf || !req)) {
		pr_err("Invalid inputs (cptvf: %p, req: %p)\n", cptvf, req);
		return -EINVAL;
	}

	info = kzalloc(sizeof(*info), GFP_KERNEL | GFP_ATOMIC);
	if (unlikely(!info)) {
		pr_err("Unable to allocate memory for info_buffer\n");
		return -ENOMEM;
	}

	cpt_req = (struct cptvf_request *)&req->req;
	ctrl = (union ctrl_info *)&req->ctrl;
	key_handle = req->handle;

	info->cptvf = cptvf;
	info->outcnt = req->outcnt;
	info->req_type = ctrl->s.req_mode;
	info->dma_mode = ctrl->s.dma_mode;
	info->dlen   = cpt_req->dlen;
	/* Add 8-bytes more for microcode completion code */
	info->rlen   = ROUNDUP8(req->rlen + COMPLETION_CODE_SIZE);

	group = ctrl->s.grp;
	ret = setup_sgio_list(cptvf, info, req);
	if (ret) {
		pr_err("Setting up SG list failed");
		goto request_cleanup;
	}

	cpt_req->dlen = info->dlen;
	info->opcode = cpt_req->opcode.flags;
	/*
	 * Get buffer for union cpt_res_s response
	 * structure and its physical address
	 */
	info->completion_addr = kzalloc(sizeof(union cpt_res_s),
					     GFP_KERNEL | GFP_ATOMIC);
	*((uint8_t *)(info->completion_addr)) = COMPLETION_CODE_INIT;
	info->comp_baddr = dma_map_single(&pdev->dev,
					       (void *)info->completion_addr,
					       sizeof(union cpt_res_s),
					       DMA_BIDIRECTIONAL);
	if (dma_mapping_error(&pdev->dev, info->comp_baddr)) {
		pr_err("mapping compptr Failed %lu\n", sizeof(union cpt_res_s));
		ret = -EFAULT;
		goto  request_cleanup;
	}

	/* Fill the VQ command */
	vq_cmd.cmd.u64 = 0;
	vq_cmd.cmd.s.opcode = cpu_to_be16(cpt_req->opcode.flags);
	vq_cmd.cmd.s.param1 = cpu_to_be16(cpt_req->param1);
	vq_cmd.cmd.s.param2 = cpu_to_be16(cpt_req->param2);
	vq_cmd.cmd.s.dlen   = cpu_to_be16(cpt_req->dlen);

	/* 64-bit swap for microcode data reads, not needed for addresses*/
	vq_cmd.cmd.u64 = cpu_to_be64(vq_cmd.cmd.u64);
	vq_cmd.dptr = info->dptr_baddr;
	vq_cmd.rptr = info->rptr_baddr;
	vq_cmd.cptr.u64 = 0;
	vq_cmd.cptr.s.grp = group;
	/* Get Pending Entry to submit command */
	/*queue = SMP_PROCESSOR_ID() % cptvf->nr_queues;*/
	/* Always queue 0, because 1 queue per VF */
	queue = 0;
	info->queue = queue;
	pqueue = &cptvf->pqinfo.queue[queue];

	if (atomic64_read(&pqueue->pending_count) > PENDING_THOLD) {
		pr_err("pending threshold reached\n");
		process_pending_queue(cptvf, &cptvf->pqinfo, queue);
	}

get_pending_entry:
	spin_lock_bh(&pqueue->lock);
	pentry = get_free_pending_entry(pqueue, cptvf->pqinfo.qlen);
	if (unlikely(!pentry)) {
		spin_unlock_bh(&pqueue->lock);
		if (clear == 0) {
			process_pending_queue(cptvf, &cptvf->pqinfo, queue);
			clear = 1;
			goto get_pending_entry;
		}
		pr_err("Get free entry failed\n");
		pr_err("queue: %d, rear: %d, front: %d\n",
		       queue, pqueue->rear, pqueue->front);
		ret = -EFAULT;
		goto request_cleanup;
	}

	pentry->done = false;
	pentry->completion_addr = info->completion_addr;
	pentry->post_arg = (void *)info;
	pentry->callback = req->callback;
	pentry->callback_arg = req->callback_arg;
	info->pentry = pentry;
	pentry->busy = true;
	atomic64_inc(&pqueue->pending_count);

	/* Send CPT command */
	info->pentry = pentry;
	info->status = ERR_REQ_PENDING;
	info->time_in = jiffies;

	/* Create the CPT_INST_S type command for HW intrepretation */
	cptinst.s.doneint = true;
	cptinst.s.res_addr = (uint64_t)info->comp_baddr;
	cptinst.s.tag = 0;
	cptinst.s.grp = 0;
	cptinst.s.wq_ptr = 0;
	cptinst.s.ei0 = vq_cmd.cmd.u64;
	cptinst.s.ei1 = vq_cmd.dptr;
	cptinst.s.ei2 = vq_cmd.rptr;
	cptinst.s.ei3 = vq_cmd.cptr.u64;

	ret = send_cpt_command(cptvf, &cptinst, queue);
	spin_unlock_bh(&pqueue->lock);
	if (unlikely(ret)) {
		spin_unlock_bh(&pqueue->lock);
		pr_err("Send command failed for AE\n");
		ret = -EFAULT;
		goto request_cleanup;
	}

	/* Non-Blocking request */
	req->request_id = (uint64_t)(info);
	req->status = -EAGAIN;

	return 0;

request_cleanup:
	pr_debug("Failed to submit CPT command\n");
	do_request_cleanup(cptvf, info);

	return ret;
}

void vq_post_process(struct cpt_vf *cptvf, uint32_t qno)
{
	if (unlikely(qno > cptvf->nr_queues)) {
		pr_err("Request for post processing on invalid pending queue: %u\n",
		       qno);
		return;
	}

	process_pending_queue(cptvf, &cptvf->pqinfo, qno);
}

int32_t cptvf_do_request(void *vfdev, struct cpt_request_info *req)
{
	struct cpt_vf *cptvf = (struct cpt_vf *)vfdev;

	if (!cpt_device_ready(cptvf)) {
		pr_err("CPT Device is not ready");
		return -ENODEV;
	}

	if ((cptvf->vftype == SE_TYPES) && (!req->ctrl.s.se_req)) {
		pr_err("CPTVF-%d of SE TYPE got AE request", cptvf->vfid);
		return -EINVAL;
	} else if ((cptvf->vftype == AE_TYPES) && (req->ctrl.s.se_req)) {
		pr_err("CPTVF-%d of AE TYPE got SE request", cptvf->vfid);
		return -EINVAL;
	}

	cptvf->reqmode = req->ctrl.s.req_mode;

	return process_request(cptvf, req);
}
