/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005, 2006, 2007 Cisco Systems.  All rights reserved.
 * Copyright (c) 2005 PathScale, Inc.  All rights reserved.
 * Copyright (c) 2006 Mellanox Technologies.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
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

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include <linux/uaccess.h>

#include "uverbs.h"
#include "core_priv.h"

struct uverbs_lock_class pd_lock_class	= { .name = "PD-uobj" };
struct uverbs_lock_class mr_lock_class	= { .name = "MR-uobj" };
struct uverbs_lock_class mw_lock_class	= { .name = "MW-uobj" };
struct uverbs_lock_class cq_lock_class	= { .name = "CQ-uobj" };
struct uverbs_lock_class qp_lock_class	= { .name = "QP-uobj" };
struct uverbs_lock_class ah_lock_class	= { .name = "AH-uobj" };
struct uverbs_lock_class srq_lock_class	= { .name = "SRQ-uobj" };
struct uverbs_lock_class xrcd_lock_class = { .name = "XRCD-uobj" };
struct uverbs_lock_class rule_lock_class = { .name = "RULE-uobj" };

/*
 * The ib_uobject locking scheme is as follows:
 *
 * - ib_uverbs_idr_lock protects the uverbs idrs themselves, so it
 *   needs to be held during all idr write operations.  When an object is
 *   looked up, a reference must be taken on the object's kref before
 *   dropping this lock.  For read operations, the rcu_read_lock()
 *   and rcu_write_lock() but similarly the kref reference is grabbed
 *   before the rcu_read_unlock().
 *
 * - Each object also has an rwsem.  This rwsem must be held for
 *   reading while an operation that uses the object is performed.
 *   For example, while registering an MR, the associated PD's
 *   uobject.mutex must be held for reading.  The rwsem must be held
 *   for writing while initializing or destroying an object.
 *
 * - In addition, each object has a "live" flag.  If this flag is not
 *   set, then lookups of the object will fail even if it is found in
 *   the idr.  This handles a reader that blocks and does not acquire
 *   the rwsem until after the object is destroyed.  The destroy
 *   operation will set the live flag to 0 and then drop the rwsem;
 *   this will allow the reader to acquire the rwsem, see that the
 *   live flag is 0, and then drop the rwsem and its reference to
 *   object.  The underlying storage will not be freed until the last
 *   reference to the object is dropped.
 */

void ib_init_uobj(struct ib_uobject *uobj, u64 user_handle,
		  struct ib_ucontext *context, struct uverbs_lock_class *c)
{
	uobj->user_handle = user_handle;
	uobj->context     = context;
	kref_init(&uobj->ref);
	init_rwsem(&uobj->mutex);
	lockdep_set_class_and_name(&uobj->mutex, &c->key, c->name);
	uobj->live        = 0;
}

static void release_uobj(struct kref *kref)
{
	kfree_rcu(container_of(kref, struct ib_uobject, ref), rcu);
}

void ib_put_uobj(struct ib_uobject *uobj)
{
	kref_put(&uobj->ref, release_uobj);
}

void ib_put_uobj_read(struct ib_uobject *uobj)
{
	up_read(&uobj->mutex);
	ib_put_uobj(uobj);
}

void ib_put_uobj_write(struct ib_uobject *uobj)
{
	up_write(&uobj->mutex);
	ib_put_uobj(uobj);
}

int ib_idr_add_uobj(struct idr *idr, struct ib_uobject *uobj)
{
	int ret;

	idr_preload(GFP_KERNEL);
	spin_lock(&ib_uverbs_idr_lock);

	ret = idr_alloc(idr, uobj, 0, 0, GFP_NOWAIT);
	if (ret >= 0)
		uobj->id = ret;

	spin_unlock(&ib_uverbs_idr_lock);
	idr_preload_end();

	return ret < 0 ? ret : 0;
}

void ib_idr_remove_uobj(struct idr *idr, struct ib_uobject *uobj)
{
	spin_lock(&ib_uverbs_idr_lock);
	idr_remove(idr, uobj->id);
	spin_unlock(&ib_uverbs_idr_lock);
}

static struct ib_uobject *__idr_get_uobj(struct idr *idr, int id,
					 struct ib_ucontext *context)
{
	struct ib_uobject *uobj;

	rcu_read_lock();
	uobj = idr_find(idr, id);
	if (uobj) {
		if (uobj->context == context)
			kref_get(&uobj->ref);
		else
			uobj = NULL;
	}
	rcu_read_unlock();

	return uobj;
}

struct ib_uobject *ib_idr_read_uobj(struct idr *idr, int id,
				    struct ib_ucontext *context, int nested)
{
	struct ib_uobject *uobj;

	uobj = __idr_get_uobj(idr, id, context);
	if (!uobj)
		return NULL;

	if (nested)
		down_read_nested(&uobj->mutex, SINGLE_DEPTH_NESTING);
	else
		down_read(&uobj->mutex);
	if (!uobj->live) {
		ib_put_uobj_read(uobj);
		return NULL;
	}

	return uobj;
}

struct ib_uobject *ib_idr_write_uobj(struct idr *idr, int id,
				     struct ib_ucontext *context)
{
	struct ib_uobject *uobj;

	uobj = __idr_get_uobj(idr, id, context);
	if (!uobj)
		return NULL;

	down_write(&uobj->mutex);
	if (!uobj->live) {
		ib_put_uobj_write(uobj);
		return NULL;
	}

	return uobj;
}

static void *idr_read_obj(struct idr *idr, int id, struct ib_ucontext *context,
			  int nested)
{
	struct ib_uobject *uobj;

	uobj = ib_idr_read_uobj(idr, id, context, nested);
	return uobj ? uobj->object : NULL;
}

struct ib_pd *ib_idr_read_pd(int pd_handle, struct ib_ucontext *context)
{
	return idr_read_obj(&ib_uverbs_pd_idr, pd_handle, context, 0);
}

void ib_put_read_pd(struct ib_pd *pd)
{
	ib_put_uobj_read(pd->uobject);
}

struct ib_cq *ib_idr_read_cq(int cq_handle, struct ib_ucontext *context, int nested)
{
	return idr_read_obj(&ib_uverbs_cq_idr, cq_handle, context, nested);
}

void ib_put_read_cq(struct ib_cq *cq)
{
	ib_put_uobj_read(cq->uobject);
}

struct ib_ah *ib_idr_read_ah(int ah_handle, struct ib_ucontext *context)
{
	return idr_read_obj(&ib_uverbs_ah_idr, ah_handle, context, 0);
}

void ib_put_read_ah(struct ib_ah *ah)
{
	ib_put_uobj_read(ah->uobject);
}

struct ib_qp *ib_idr_read_qp(int qp_handle, struct ib_ucontext *context)
{
	return idr_read_obj(&ib_uverbs_qp_idr, qp_handle, context, 0);
}

struct ib_qp *ib_idr_write_qp(int qp_handle, struct ib_ucontext *context)
{
	struct ib_uobject *uobj;

	uobj = ib_idr_write_uobj(&ib_uverbs_qp_idr, qp_handle, context);
	return uobj ? uobj->object : NULL;
}

void ib_put_read_qp(struct ib_qp *qp)
{
	ib_put_uobj_read(qp->uobject);
}

void ib_put_write_qp(struct ib_qp *qp)
{
	ib_put_uobj_write(qp->uobject);
}

struct ib_srq *ib_idr_read_srq(int srq_handle, struct ib_ucontext *context)
{
	return idr_read_obj(&ib_uverbs_srq_idr, srq_handle, context, 0);
}

void ib_put_read_srq(struct ib_srq *srq)
{
	ib_put_uobj_read(srq->uobject);
}

struct ib_xrcd *ib_idr_read_xrcd(int xrcd_handle, struct ib_ucontext *context,
				 struct ib_uobject **uobj)
{
	*uobj = ib_idr_read_uobj(&ib_uverbs_xrcd_idr, xrcd_handle, context, 0);
	return *uobj ? (*uobj)->object : NULL;
}

void ib_put_xrcd_read(struct ib_uobject *uobj)
{
	ib_put_uobj_read(uobj);
}

