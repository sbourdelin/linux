/*
 * Copyright (c) 2015 Mellanox Technologies Ltd. All rights reserved.
 * Copyright (c) 2015 System Fabric Works, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *	   Redistribution and use in source and binary forms, with or
 *	   without modification, are permitted provided that the following
 *	   conditions are met:
 *
 *	- Redistributions of source code must retain the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer.
 *
 *	- Redistributions in binary form must reproduce the above
 *	  copyright notice, this list of conditions and the following
 *	  disclaimer in the documentation and/or other materials
 *	  provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "rvt_loc.h"
#include "rvt_queue.h"

int rvt_cq_chk_attr(struct rvt_dev *rvt, struct rvt_cq *cq,
		    int cqe, int comp_vector, struct ib_udata *udata)
{
	int count;

	if (cqe <= 0) {
		pr_warn("cqe(%d) <= 0\n", cqe);
		goto err1;
	}

	if (cqe > rvt->attr.max_cqe) {
		pr_warn("cqe(%d) > max_cqe(%d)\n",
			cqe, rvt->attr.max_cqe);
		goto err1;
	}

	if (cq) {
		count = queue_count(cq->queue);
		if (cqe < count) {
			pr_warn("cqe(%d) < current # elements in queue (%d)",
				cqe, count);
			goto err1;
		}
	}

	return 0;

err1:
	return -EINVAL;
}

static void rvt_send_complete(unsigned long data)
{
	struct rvt_cq *cq = (struct rvt_cq *)data;

	cq->ibcq.comp_handler(&cq->ibcq, cq->ibcq.cq_context);
}

int rvt_cq_from_init(struct rvt_dev *rvt, struct rvt_cq *cq, int cqe,
		     int comp_vector, struct ib_ucontext *context,
		     struct ib_udata *udata)
{
	int err;

	cq->queue = rvt_queue_init(rvt, &cqe,
				   sizeof(struct rvt_cqe));
	if (!cq->queue) {
		pr_warn("unable to create cq\n");
		return -ENOMEM;
	}

	err = do_mmap_info(rvt, udata, false, context, cq->queue->buf,
			   cq->queue->buf_size, &cq->queue->ip);
	if (err) {
		kvfree(cq->queue->buf);
		kfree(cq->queue);
		return err;
	}

	if (udata)
		cq->is_user = 1;

	tasklet_init(&cq->comp_task, rvt_send_complete, (unsigned long)cq);

	spin_lock_init(&cq->cq_lock);
	cq->ibcq.cqe = cqe;
	return 0;
}

int rvt_cq_resize_queue(struct rvt_cq *cq, int cqe, struct ib_udata *udata)
{
	int err;

	err = rvt_queue_resize(cq->queue, (unsigned int *)&cqe,
			       sizeof(struct rvt_cqe),
			       cq->queue->ip ? cq->queue->ip->context : NULL,
			       udata, NULL, &cq->cq_lock);
	if (!err)
		cq->ibcq.cqe = cqe;

	return err;
}

int rvt_cq_post(struct rvt_cq *cq, struct rvt_cqe *cqe, int solicited)
{
	struct ib_event ev;
	unsigned long flags;

	spin_lock_irqsave(&cq->cq_lock, flags);

	if (unlikely(queue_full(cq->queue))) {
		spin_unlock_irqrestore(&cq->cq_lock, flags);
		if (cq->ibcq.event_handler) {
			ev.device = cq->ibcq.device;
			ev.element.cq = &cq->ibcq;
			ev.event = IB_EVENT_CQ_ERR;
			cq->ibcq.event_handler(&ev, cq->ibcq.cq_context);
		}

		return -EBUSY;
	}

	memcpy(producer_addr(cq->queue), cqe, sizeof(*cqe));

	/* make sure all changes to the CQ are written before we update the
	 * producer pointer
	 */
	smp_wmb();

	advance_producer(cq->queue);
	spin_unlock_irqrestore(&cq->cq_lock, flags);

	if ((cq->notify == IB_CQ_NEXT_COMP) ||
	    (cq->notify == IB_CQ_SOLICITED && solicited)) {
		cq->notify++;
		tasklet_schedule(&cq->comp_task);
	}

	return 0;
}

void rvt_cq_cleanup(void *arg)
{
	struct rvt_cq *cq = arg;

	if (cq->queue)
		rvt_queue_cleanup(cq->queue);
}
