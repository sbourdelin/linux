/*
 * Copyright (c) 2016, Mellanox Technologies inc.  All rights reserved.
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

#include <rdma/ib_verbs.h>
#include "uidr.h"
#include "uobject.h"

int idr_add_uobj(struct ib_uobject *uobj)
{
	int ret;

	idr_preload(GFP_KERNEL);
	spin_lock(&uobj->context->device->idr_lock);

	ret = idr_alloc(&uobj->context->device->idr, uobj, 0, 0, GFP_NOWAIT);
	if (ret >= 0)
		uobj->id = ret;

	spin_unlock(&uobj->context->device->idr_lock);
	idr_preload_end();

	return ret < 0 ? ret : 0;
}

void idr_remove_uobj(struct ib_uobject *uobj)
{
	spin_lock(&uobj->context->device->idr_lock);
	idr_remove(&uobj->context->device->idr, uobj->id);
	spin_unlock(&uobj->context->device->idr_lock);
}

static struct ib_uobject *__idr_get_uobj(int id, struct ib_ucontext *context)
{
	struct ib_uobject *uobj;

	rcu_read_lock();
	uobj = idr_find(&context->device->idr, id);
	if (uobj) {
		if (uobj->context == context)
			kref_get(&uobj->ref);
		else
			uobj = NULL;
	}
	rcu_read_unlock();

	return uobj;
}

static struct ib_uobject *idr_read_uobj(int id, struct ib_ucontext *context,
					int nested)
{
	struct ib_uobject *uobj;

	uobj = __idr_get_uobj(id, context);
	if (!uobj)
		return NULL;

	if (nested)
		down_read_nested(&uobj->mutex, SINGLE_DEPTH_NESTING);
	else
		down_read(&uobj->mutex);
	if (!uobj->live) {
		put_uobj_read(uobj);
		return NULL;
	}

	return uobj;
}

struct ib_uobject *idr_write_uobj(int id, struct ib_ucontext *context)
{
	struct ib_uobject *uobj;

	uobj = __idr_get_uobj(id, context);
	if (!uobj)
		return NULL;

	down_write(&uobj->mutex);
	if (!uobj->live) {
		put_uobj_write(uobj);
		return NULL;
	}

	return uobj;
}

static void *idr_read_obj(int id, struct ib_ucontext *context,
			  int nested)
{
	struct ib_uobject *uobj;

	uobj = idr_read_uobj(id, context, nested);
	return uobj ? uobj->object : NULL;
}

struct ib_pd *idr_read_pd(int pd_handle, struct ib_ucontext *context)
{
	return idr_read_obj(pd_handle, context, 0);
}

struct ib_cq *idr_read_cq(int cq_handle, struct ib_ucontext *context,
			  int nested)
{
	return idr_read_obj(cq_handle, context, nested);
}

struct ib_ah *idr_read_ah(int ah_handle, struct ib_ucontext *context)
{
	return idr_read_obj(ah_handle, context, 0);
}

struct ib_qp *idr_read_qp(int qp_handle, struct ib_ucontext *context)
{
	return idr_read_obj(qp_handle, context, 0);
}

struct ib_qp *idr_write_qp(int qp_handle, struct ib_ucontext *context)
{
	struct ib_uobject *uobj;

	uobj = idr_write_uobj(qp_handle, context);
	return uobj ? uobj->object : NULL;
}

struct ib_srq *idr_read_srq(int srq_handle, struct ib_ucontext *context)
{
	return idr_read_obj(srq_handle, context, 0);
}

struct ib_xrcd *idr_read_xrcd(int xrcd_handle, struct ib_ucontext *context,
			      struct ib_uobject **uobj)
{
	*uobj = idr_read_uobj(xrcd_handle, context, 0);
	return *uobj ? (*uobj)->object : NULL;
}

