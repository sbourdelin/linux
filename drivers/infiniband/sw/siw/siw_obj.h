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

#ifndef _SIW_OBJ_H
#define _SIW_OBJ_H

#include <linux/idr.h>
#include <linux/rwsem.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/semaphore.h>

#include <rdma/ib_verbs.h>

#include "siw_debug.h"

extern void siw_free_qp(struct kref *ref);
extern void siw_free_pd(struct kref *ref);
extern void siw_free_cq(struct kref *ref);
extern void siw_free_mem(struct kref *ref);

static inline struct siw_device *siw_dev_base2siw(struct ib_device *base_dev)
{
	return container_of(base_dev, struct siw_device, base_dev);
}

static inline struct siw_mr *siw_mr_base2siw(struct ib_mr *base_mr)
{
	return container_of(base_mr, struct siw_mr, base_mr);
}

static inline void siw_cq_get(struct siw_cq *cq)
{
	kref_get(&cq->hdr.ref);
	siw_dbg_obj(cq, "new refcount: %d\n", refcount_read(&cq->hdr.ref));
}
static inline void siw_qp_get(struct siw_qp *qp)
{
	kref_get(&qp->hdr.ref);
	siw_dbg_obj(qp, "new refcount: %d\n", refcount_read(&qp->hdr.ref));
}

static inline void siw_pd_get(struct siw_pd *pd)
{
	kref_get(&pd->hdr.ref);
	siw_dbg_obj(pd, "new refcount: %d\n", refcount_read(&pd->hdr.ref));
}

static inline void siw_mem_get(struct siw_mem *mem)
{
	kref_get(&mem->hdr.ref);
	siw_dbg_obj(mem, "new refcount: %d\n", refcount_read(&mem->hdr.ref));
}

static inline void siw_mem_put(struct siw_mem *mem)
{
	siw_dbg_obj(mem, "old refcount: %d\n", refcount_read(&mem->hdr.ref));
	kref_put(&mem->hdr.ref, siw_free_mem);
}

static inline void siw_unref_mem_sgl(struct siw_mem **mem,
				     unsigned int num_sge)
{
	while (num_sge) {
		if (*mem == NULL)
			break;

		siw_mem_put(*mem);
		*mem = NULL;
		mem++; num_sge--;
	}
}

static inline struct siw_objhdr *siw_get_obj(struct idr *idr, int id)
{
	struct siw_objhdr *obj = idr_find(idr, id);

	if (obj)
		kref_get(&obj->ref);

	return obj;
}

static inline struct siw_cq *siw_cq_id2obj(struct siw_device *sdev, int id)
{
	struct siw_objhdr *obj = siw_get_obj(&sdev->cq_idr, id);

	if (obj)
		return container_of(obj, struct siw_cq, hdr);

	return NULL;
}

static inline struct siw_qp *siw_qp_id2obj(struct siw_device *sdev, int id)
{
	struct siw_objhdr *obj = siw_get_obj(&sdev->qp_idr, id);

	if (obj)
		return container_of(obj, struct siw_qp, hdr);

	return NULL;
}

static inline void siw_cq_put(struct siw_cq *cq)
{
	siw_dbg_obj(cq, "old refcount: %d\n", refcount_read(&cq->hdr.ref));
	kref_put(&cq->hdr.ref, siw_free_cq);
}

static inline void siw_qp_put(struct siw_qp *qp)
{
	siw_dbg_obj(qp, "old refcount: %d\n", refcount_read(&qp->hdr.ref));
	kref_put(&qp->hdr.ref, siw_free_qp);
}

static inline void siw_pd_put(struct siw_pd *pd)
{
	siw_dbg_obj(pd, "old refcount: %d\n", refcount_read(&pd->hdr.ref));
	kref_put(&pd->hdr.ref, siw_free_pd);
}

/*
 * siw_mem_id2obj()
 *
 * resolves memory from stag given by id. might be called from:
 * o process context before sending out of sgl, or
 * o in softirq when resolving target memory
 */
static inline struct siw_mem *siw_mem_id2obj(struct siw_device *sdev, int id)
{
	struct siw_objhdr *obj;
	struct siw_mem *mem = NULL;

	rcu_read_lock();
	obj = siw_get_obj(&sdev->mem_idr, id);
	rcu_read_unlock();

	if (likely(obj)) {
		mem = container_of(obj, struct siw_mem, hdr);
		siw_dbg_obj(mem, "new refcount: %d\n",
			    refcount_read(&obj->ref));
	}
	return mem;
}

extern void siw_remove_obj(spinlock_t *lock, struct idr *idr,
				struct siw_objhdr *hdr);

extern void siw_objhdr_init(struct siw_objhdr *hdr);
extern void siw_idr_init(struct siw_device *dev);
extern void siw_idr_release(struct siw_device *dev);

extern struct siw_cq *siw_cq_id2obj(struct siw_device *dev, int id);
extern struct siw_qp *siw_qp_id2obj(struct siw_device *dev, int id);
extern struct siw_mem *siw_mem_id2obj(struct siw_device *dev, int id);

extern int siw_qp_add(struct siw_device *dev, struct siw_qp *qp);
extern int siw_cq_add(struct siw_device *dev, struct siw_cq *cq);
extern int siw_pd_add(struct siw_device *dev, struct siw_pd *pd);
extern int siw_mem_add(struct siw_device *dev, struct siw_mem *mem);

extern struct siw_wqe *siw_freeq_wqe_get(struct siw_qp *qp);

extern int siw_invalidate_stag(struct siw_pd *pd, u32 stag);
#endif
