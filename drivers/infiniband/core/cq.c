/*
 * Copyright (c) 2015 HGST, a Western Digital Company.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#include <linux/module.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <rdma/ib_verbs.h>
#include <linux/blk_dim.h>

/* # of WCs to poll for with a single call to ib_poll_cq */
#define IB_POLL_BATCH			16
#define IB_POLL_BATCH_DIRECT		8

/* # of WCs to iterate over before yielding */
#define IB_POLL_BUDGET_IRQ		256
#define IB_POLL_BUDGET_WORKQUEUE	65536

#define IB_POLL_FLAGS \
	(IB_CQ_NEXT_COMP | IB_CQ_REPORT_MISSED_EVENTS)

static bool use_am = true;
module_param(use_am, bool, 0444);
MODULE_PARM_DESC(use_am, "Use cq adaptive moderation");

static int ib_cq_dim_modify_cq(struct ib_cq *cq, unsigned short level)
{
	u16 usec = blk_dim_prof[level].usec;
	u16 comps = blk_dim_prof[level].comps;

	return cq->device->modify_cq(cq, comps, usec);
}

static void update_cq_moderation(struct dim *dim, struct ib_cq *cq)
{
	dim->state = DIM_START_MEASURE;

	ib_cq_dim_modify_cq(cq, dim->profile_ix);
}

static void ib_cq_blk_dim_workqueue_work(struct work_struct *w)
{
	struct dim *dim = container_of(w, struct dim, work);
	struct ib_cq *cq = container_of(dim, struct ib_cq, workqueue_poll.dim);

	update_cq_moderation(dim, cq);
}

static void ib_cq_blk_dim_irqpoll_work(struct work_struct *w)
{
	struct dim *dim = container_of(w, struct dim, work);
	struct irq_poll *iop = container_of(dim, struct irq_poll, dim);
	struct ib_cq *cq = container_of(iop, struct ib_cq, iop);

	update_cq_moderation(dim, cq);
}

void blk_dim_init(struct dim *dim, work_func_t func)
{
	memset(dim, 0, sizeof(*dim));
	dim->state = DIM_START_MEASURE;
	dim->tune_state = DIM_GOING_RIGHT;
	dim->profile_ix = BLK_DIM_START_PROFILE;
	INIT_WORK(&dim->work, func);
}

static int __ib_process_cq(struct ib_cq *cq, int budget, struct ib_wc *wcs,
			   int batch)
{
	int i, n, completed = 0;

	/*
	 * budget might be (-1) if the caller does not
	 * want to bound this call, thus we need unsigned
	 * minimum here.
	 */
	while ((n = ib_poll_cq(cq, min_t(u32, batch,
					 budget - completed), wcs)) > 0) {
		for (i = 0; i < n; i++) {
			struct ib_wc *wc = &wcs[i];

			if (wc->wr_cqe)
				wc->wr_cqe->done(cq, wc);
			else
				WARN_ON_ONCE(wc->status == IB_WC_SUCCESS);
		}

		completed += n;

		if (n != batch || (budget != -1 && completed >= budget))
			break;
	}

	return completed;
}

/**
 * ib_process_direct_cq - process a CQ in caller context
 * @cq:		CQ to process
 * @budget:	number of CQEs to poll for
 *
 * This function is used to process all outstanding CQ entries.
 * It does not offload CQ processing to a different context and does
 * not ask for completion interrupts from the HCA.
 * Using direct processing on CQ with non IB_POLL_DIRECT type may trigger
 * concurrent processing.
 *
 * Note: do not pass -1 as %budget unless it is guaranteed that the number
 * of completions that will be processed is small.
 */
int ib_process_cq_direct(struct ib_cq *cq, int budget)
{
	struct ib_wc wcs[IB_POLL_BATCH_DIRECT];

	return __ib_process_cq(cq, budget, wcs, IB_POLL_BATCH_DIRECT);
}
EXPORT_SYMBOL(ib_process_cq_direct);

static void ib_cq_completion_direct(struct ib_cq *cq, void *private)
{
	WARN_ONCE(1, "got unsolicited completion for CQ 0x%p\n", cq);
}

static int ib_poll_handler(struct irq_poll *iop, int budget)
{
	struct ib_cq *cq = container_of(iop, struct ib_cq, iop);
	int completed;

	completed = __ib_process_cq(cq, budget, cq->wc, IB_POLL_BATCH);
	if (completed < budget) {
		irq_poll_complete(&cq->iop);
		if (ib_req_notify_cq(cq, IB_POLL_FLAGS) > 0)
			irq_poll_sched(&cq->iop);
	}

	return completed;
}

static void ib_cq_completion_softirq(struct ib_cq *cq, void *private)
{
	irq_poll_sched(&cq->iop);
}

static void ib_cq_poll_work(struct work_struct *work)
{
	struct ib_cq *cq = container_of(work, struct ib_cq, workqueue_poll.work);
	int completed;
	struct dim_sample e_sample;
	struct dim_sample *m_sample = &cq->workqueue_poll.dim.measuring_sample;

	completed = __ib_process_cq(cq, IB_POLL_BUDGET_WORKQUEUE, cq->wc,
				    IB_POLL_BATCH);

	if (cq->workqueue_poll.dim_used)
		dim_create_sample(m_sample->event_ctr + 1, m_sample->pkt_ctr, m_sample->byte_ctr,
							m_sample->comp_ctr + completed, &e_sample);

	if (completed >= IB_POLL_BUDGET_WORKQUEUE ||
	    ib_req_notify_cq(cq, IB_POLL_FLAGS) > 0)
		queue_work(cq->comp_wq, &cq->workqueue_poll.work);
	else if (cq->workqueue_poll.dim_used)
		blk_dim(&cq->workqueue_poll.dim, e_sample);
}

static void ib_cq_completion_workqueue(struct ib_cq *cq, void *private)
{
	queue_work(cq->comp_wq, &cq->workqueue_poll.work);
}

/**
 * __ib_alloc_cq - allocate a completion queue
 * @dev:		device to allocate the CQ for
 * @private:		driver private data, accessible from cq->cq_context
 * @nr_cqe:		number of CQEs to allocate
 * @comp_vector:	HCA completion vectors for this CQ
 * @poll_ctx:		context to poll the CQ from.
 * @caller:		module owner name.
 *
 * This is the proper interface to allocate a CQ for in-kernel users. A
 * CQ allocated with this interface will automatically be polled from the
 * specified context. The ULP must use wr->wr_cqe instead of wr->wr_id
 * to use this CQ abstraction.
 */
struct ib_cq *__ib_alloc_cq(struct ib_device *dev, void *private,
			    int nr_cqe, int comp_vector,
			    enum ib_poll_context poll_ctx, const char *caller)
{
	struct ib_cq_init_attr cq_attr = {
		.cqe		= nr_cqe,
		.comp_vector	= comp_vector,
	};
	struct ib_cq *cq;
	int ret = -ENOMEM;

	cq = dev->ops.create_cq(dev, &cq_attr, NULL, NULL);
	if (IS_ERR(cq))
		return cq;

	cq->device = dev;
	cq->uobject = NULL;
	cq->event_handler = NULL;
	cq->cq_context = private;
	cq->poll_ctx = poll_ctx;
	atomic_set(&cq->usecnt, 0);

	cq->wc = kmalloc_array(IB_POLL_BATCH, sizeof(*cq->wc), GFP_KERNEL);
	if (!cq->wc)
		goto out_destroy_cq;

	cq->res.type = RDMA_RESTRACK_CQ;
	rdma_restrack_set_task(&cq->res, caller);
	rdma_restrack_kadd(&cq->res);

	switch (cq->poll_ctx) {
	case IB_POLL_DIRECT:
		cq->comp_handler = ib_cq_completion_direct;
		break;
	case IB_POLL_SOFTIRQ:
		cq->comp_handler = ib_cq_completion_softirq;

		irq_poll_init(&cq->iop, IB_POLL_BUDGET_IRQ, ib_poll_handler);
		if (cq->device->modify_cq && use_am) {
			blk_dim_init(&cq->iop.dim, ib_cq_blk_dim_irqpoll_work);
			cq->iop.dim_used = true;
		}
		ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
		break;
	case IB_POLL_WORKQUEUE:
	case IB_POLL_UNBOUND_WORKQUEUE:
		cq->comp_handler = ib_cq_completion_workqueue;
		INIT_WORK(&cq->workqueue_poll.work, ib_cq_poll_work);
		if (cq->device->modify_cq && use_am) {
			blk_dim_init(&cq->workqueue_poll.dim, ib_cq_blk_dim_workqueue_work);
			cq->workqueue_poll.dim_used = true;
		}
		ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
		cq->comp_wq = (cq->poll_ctx == IB_POLL_WORKQUEUE) ?
				ib_comp_wq : ib_comp_unbound_wq;
		break;
	default:
		ret = -EINVAL;
		goto out_free_wc;
	}

	return cq;

out_free_wc:
	kfree(cq->wc);
	rdma_restrack_del(&cq->res);
out_destroy_cq:
	cq->device->ops.destroy_cq(cq);
	return ERR_PTR(ret);
}
EXPORT_SYMBOL(__ib_alloc_cq);

/**
 * ib_free_cq - free a completion queue
 * @cq:		completion queue to free.
 */
void ib_free_cq(struct ib_cq *cq)
{
	int ret;

	if (WARN_ON_ONCE(atomic_read(&cq->usecnt)))
		return;

	switch (cq->poll_ctx) {
	case IB_POLL_DIRECT:
		break;
	case IB_POLL_SOFTIRQ:
		irq_poll_disable(&cq->iop);
		break;
	case IB_POLL_WORKQUEUE:
	case IB_POLL_UNBOUND_WORKQUEUE:
		cancel_work_sync(&cq->workqueue_poll.work);
		if (cq->workqueue_poll.dim_used)
			flush_work(&cq->iop.dim.work);
		break;
	default:
		WARN_ON_ONCE(1);
	}

	kfree(cq->wc);
	rdma_restrack_del(&cq->res);
	ret = cq->device->ops.destroy_cq(cq);
	WARN_ON_ONCE(ret);
}
EXPORT_SYMBOL(ib_free_cq);
