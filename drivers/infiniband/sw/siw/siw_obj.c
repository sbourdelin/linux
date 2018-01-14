/*
 * Software iWARP device driver
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2017, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses. You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
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

#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/vmalloc.h>

#include "siw.h"
#include "siw_obj.h"
#include "siw_cm.h"

void siw_objhdr_init(struct siw_objhdr *hdr)
{
	kref_init(&hdr->ref);
}

void siw_idr_init(struct siw_device *sdev)
{
	spin_lock_init(&sdev->idr_lock);

	idr_init(&sdev->qp_idr);
	idr_init(&sdev->cq_idr);
	idr_init(&sdev->pd_idr);
	idr_init(&sdev->mem_idr);
}

void siw_idr_release(struct siw_device *sdev)
{
	idr_destroy(&sdev->qp_idr);
	idr_destroy(&sdev->cq_idr);
	idr_destroy(&sdev->pd_idr);
	idr_destroy(&sdev->mem_idr);
}

static int siw_add_obj(spinlock_t *lock, struct idr *idr,
		       struct siw_objhdr *obj)
{
	unsigned long flags;
	int id, pre_id;

	do {
		get_random_bytes(&pre_id, sizeof(pre_id));
		pre_id &= 0xffffff;
	} while (pre_id == 0);
again:
	spin_lock_irqsave(lock, flags);
	id = idr_alloc(idr, obj, pre_id, 0xffffff - 1, GFP_KERNEL);
	spin_unlock_irqrestore(lock, flags);

	if (id > 0) {
		siw_objhdr_init(obj);
		obj->id = id;
	} else if (id == -ENOSPC && pre_id != 1) {
		pre_id = 1;
		goto again;
	} else {
		pr_warn("SIW: idr new object failed!\n");
	}
	return id > 0 ? 0 : id;
}

int siw_qp_add(struct siw_device *sdev, struct siw_qp *qp)
{
	int rv = siw_add_obj(&sdev->idr_lock, &sdev->qp_idr, &qp->hdr);

	if (!rv) {
		qp->hdr.sdev = sdev;
		siw_dbg_obj(qp, "new qp\n");
	}
	return rv;
}

int siw_cq_add(struct siw_device *sdev, struct siw_cq *cq)
{
	int rv = siw_add_obj(&sdev->idr_lock, &sdev->cq_idr, &cq->hdr);

	if (!rv) {
		cq->hdr.sdev = sdev;
		siw_dbg_obj(cq, "new cq\n");
	}
	return rv;
}

int siw_pd_add(struct siw_device *sdev, struct siw_pd *pd)
{
	int rv = siw_add_obj(&sdev->idr_lock, &sdev->pd_idr, &pd->hdr);

	if (!rv) {
		pd->hdr.sdev = sdev;
		siw_dbg_obj(pd, "new pd\n");
	}
	return rv;
}

/*
 * Stag lookup is based on its index part only (24 bits).
 * The code avoids special Stag of zero and tries to randomize
 * STag values between 1 and SIW_STAG_MAX.
 */
int siw_mem_add(struct siw_device *sdev, struct siw_mem *m)
{
	unsigned long flags;
	int id, pre_id;

	do {
		get_random_bytes(&pre_id, sizeof(pre_id));
		pre_id &= 0xffffff;
	} while (pre_id == 0);
again:
	spin_lock_irqsave(&sdev->idr_lock, flags);
	id = idr_alloc(&sdev->mem_idr, m, pre_id, SIW_STAG_MAX, GFP_KERNEL);
	spin_unlock_irqrestore(&sdev->idr_lock, flags);

	if (id == -ENOSPC || id > SIW_STAG_MAX) {
		if (pre_id == 1) {
			pr_warn("SIW: idr new memory object failed!\n");
			return -ENOSPC;
		}
		pre_id = 1;
		goto again;
	}
	siw_objhdr_init(&m->hdr);
	m->hdr.id = id;
	m->hdr.sdev = sdev;

	siw_dbg_obj(m, "new mem\n");

	return 0;
}

void siw_remove_obj(spinlock_t *lock, struct idr *idr,
		      struct siw_objhdr *hdr)
{
	unsigned long	flags;

	spin_lock_irqsave(lock, flags);
	idr_remove(idr, hdr->id);
	spin_unlock_irqrestore(lock, flags);
}

/********** routines to put objs back and free if no ref left *****/

void siw_free_cq(struct kref *ref)
{
	struct siw_cq *cq =
		(container_of(container_of(ref, struct siw_objhdr, ref),
			      struct siw_cq, hdr));

	siw_dbg_obj(cq, "free cq\n");

	atomic_dec(&cq->hdr.sdev->num_cq);
	if (cq->queue)
		vfree(cq->queue);
	kfree(cq);
}

void siw_free_qp(struct kref *ref)
{
	struct siw_qp *qp =
		container_of(container_of(ref, struct siw_objhdr, ref),
			     struct siw_qp, hdr);
	struct siw_device *sdev = qp->hdr.sdev;
	unsigned long flags;

	siw_dbg_obj(qp, "free qp\n");

	if (qp->cep)
		siw_cep_put(qp->cep);

	siw_remove_obj(&sdev->idr_lock, &sdev->qp_idr, &qp->hdr);

	spin_lock_irqsave(&sdev->idr_lock, flags);
	list_del(&qp->devq);
	spin_unlock_irqrestore(&sdev->idr_lock, flags);

	if (qp->sendq)
		vfree(qp->sendq);
	if (qp->recvq)
		vfree(qp->recvq);
	if (qp->irq)
		vfree(qp->irq);
	if (qp->orq)
		vfree(qp->orq);

	siw_put_tx_cpu(qp->tx_cpu);

	atomic_dec(&sdev->num_qp);
	kfree(qp);
}

void siw_free_pd(struct kref *ref)
{
	struct siw_pd	*pd =
		container_of(container_of(ref, struct siw_objhdr, ref),
			     struct siw_pd, hdr);

	siw_dbg_obj(pd, "free pd\n");

	atomic_dec(&pd->hdr.sdev->num_pd);
	kfree(pd);
}

void siw_free_mem(struct kref *ref)
{
	struct siw_mem *m;
	struct siw_device *sdev;

	m = container_of(container_of(ref, struct siw_objhdr, ref),
			 struct siw_mem, hdr);
	sdev = m->hdr.sdev;

	siw_dbg_obj(m, "free mem\n");

	atomic_dec(&sdev->num_mr);

	if (SIW_MEM_IS_MW(m)) {
		struct siw_mw *mw = container_of(m, struct siw_mw, mem);

		kfree_rcu(mw, rcu);
	} else {
		struct siw_mr *mr = container_of(m, struct siw_mr, mem);
		unsigned long flags;

		siw_dbg(m->hdr.sdev, "[MEM %d]: has pbl: %s\n",
			OBJ_ID(m), mr->mem.is_pbl ? "y" : "n");

		if (mr->pd)
			siw_pd_put(mr->pd);

		if (mr->mem_obj) {
			if (mr->mem.is_pbl == 0)
				siw_umem_release(mr->umem);
			else
				siw_pbl_free(mr->pbl);

		}
		spin_lock_irqsave(&sdev->idr_lock, flags);
		list_del(&mr->devq);
		spin_unlock_irqrestore(&sdev->idr_lock, flags);

		kfree_rcu(mr, rcu);
	}
}

/***** routines for WQE handling ***/

void siw_wqe_put_mem(struct siw_wqe *wqe, enum siw_opcode op)
{
	switch (op) {

	case SIW_OP_SEND:
	case SIW_OP_WRITE:
	case SIW_OP_SEND_WITH_IMM:
	case SIW_OP_SEND_REMOTE_INV:
	case SIW_OP_READ:
	case SIW_OP_READ_LOCAL_INV:
		if (!(wqe->sqe.flags & SIW_WQE_INLINE))
			siw_unref_mem_sgl(wqe->mem, wqe->sqe.num_sge);
		break;

	case SIW_OP_RECEIVE:
		siw_unref_mem_sgl(wqe->mem, wqe->rqe.num_sge);
		break;

	case SIW_OP_READ_RESPONSE:
		siw_unref_mem_sgl(wqe->mem, 1);
		break;

	default:
		/*
		 * SIW_OP_INVAL_STAG and SIW_OP_REG_MR
		 * do not hold memory references
		 */
		break;
	}
}

int siw_invalidate_stag(struct siw_pd *pd, u32 stag)
{
	u32 stag_idx = stag >> 8;
	struct siw_mem *mem = siw_mem_id2obj(pd->hdr.sdev, stag_idx);
	int rv = 0;

	if (unlikely(!mem)) {
		siw_dbg(pd->hdr.sdev, "stag %u unknown\n", stag_idx);
		return -EINVAL;
	}
	if (unlikely(siw_mem2mr(mem)->pd != pd)) {
		siw_dbg(pd->hdr.sdev, "pd mismatch for stag %u\n", stag_idx);
		rv = -EACCES;
		goto out;
	}
	/*
	 * Per RDMA verbs definition, an STag may already be in invalid
	 * state if invalidation is requested. So no state check here.
	 */
	mem->stag_valid = 0;

	siw_dbg(pd->hdr.sdev, "stag %u now valid\n", stag_idx);
out:
	siw_mem_put(mem);
	return rv;
}
