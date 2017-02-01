/*
 * Copyright (c) 2017, Mellanox Technologies inc.  All rights reserved.
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

#include <rdma/uverbs_std_types.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>
#include <linux/bug.h>
#include <linux/file.h>
#include "rdma_core.h"
#include "uverbs.h"

void uverbs_free_ah(struct ib_uobject *uobject)
{
	ib_destroy_ah((struct ib_ah *)uobject->object);
}

void uverbs_free_flow(struct ib_uobject *uobject)
{
	ib_destroy_flow((struct ib_flow *)uobject->object);
}

void uverbs_free_mw(struct ib_uobject *uobject)
{
	uverbs_dealloc_mw((struct ib_mw *)uobject->object);
}

void uverbs_free_qp(struct ib_uobject *uobject)
{
	struct ib_qp *qp = uobject->object;
	struct ib_uqp_object *uqp =
		container_of(uobject, struct ib_uqp_object, uevent.uobject);

	if (qp == qp->real_qp)
		ib_uverbs_detach_umcast(qp, uqp);
	ib_destroy_qp(qp);
	ib_uverbs_release_uevent(uobject->context->ufile, &uqp->uevent);
}

void uverbs_free_rwq_ind_tbl(struct ib_uobject *uobject)
{
	struct ib_rwq_ind_table *rwq_ind_tbl = uobject->object;
	struct ib_wq **ind_tbl = rwq_ind_tbl->ind_tbl;

	ib_destroy_rwq_ind_table(rwq_ind_tbl);
	kfree(ind_tbl);
}

void uverbs_free_wq(struct ib_uobject *uobject)
{
	struct ib_wq *wq = uobject->object;
	struct ib_uwq_object *uwq =
		container_of(uobject, struct ib_uwq_object, uevent.uobject);

	ib_destroy_wq(wq);
	ib_uverbs_release_uevent(uobject->context->ufile, &uwq->uevent);
}

void uverbs_free_srq(struct ib_uobject *uobject)
{
	struct ib_srq *srq = uobject->object;
	struct ib_uevent_object *uevent =
		container_of(uobject, struct ib_uevent_object, uobject);

	ib_destroy_srq(srq);
	ib_uverbs_release_uevent(uobject->context->ufile, uevent);
}

void uverbs_free_cq(struct ib_uobject *uobject)
{
	struct ib_cq *cq = uobject->object;
	struct ib_uverbs_event_file *ev_file = cq->cq_context;
	struct ib_ucq_object *ucq =
		container_of(uobject, struct ib_ucq_object, uobject);

	ib_destroy_cq(cq);
	ib_uverbs_release_ucq(uobject->context->ufile, ev_file, ucq);
}

void uverbs_free_mr(struct ib_uobject *uobject)
{
	ib_dereg_mr((struct ib_mr *)uobject->object);
}

void uverbs_free_xrcd(struct ib_uobject *uobject)
{
	struct ib_xrcd *xrcd = uobject->object;

	mutex_lock(&uobject->context->ufile->device->xrcd_tree_mutex);
	ib_uverbs_dealloc_xrcd(uobject->context->ufile->device, xrcd);
	mutex_unlock(&uobject->context->ufile->device->xrcd_tree_mutex);
}

void uverbs_free_pd(struct ib_uobject *uobject)
{
	ib_dealloc_pd((struct ib_pd *)uobject->object);
}

const struct uverbs_obj_idr_type uverbs_type_attrs_cq =
	UVERBS_TYPE_ALLOC_IDR_SZ(sizeof(struct ib_ucq_object), 0,
				 uverbs_free_cq);

const struct uverbs_obj_idr_type uverbs_type_attrs_qp =
	UVERBS_TYPE_ALLOC_IDR_SZ(sizeof(struct ib_uqp_object), 0,
				 uverbs_free_qp);

const struct uverbs_obj_idr_type uverbs_type_attrs_mw =
	UVERBS_TYPE_ALLOC_IDR(0, uverbs_free_mw);

const struct uverbs_obj_idr_type uverbs_type_attrs_mr =
	/* 1 is used in order to free the MR after all the MWs */
	UVERBS_TYPE_ALLOC_IDR(1, uverbs_free_mr);

const struct uverbs_obj_idr_type uverbs_type_attrs_srq =
	UVERBS_TYPE_ALLOC_IDR_SZ(sizeof(struct ib_usrq_object), 0,
				 uverbs_free_srq);

const struct uverbs_obj_idr_type uverbs_type_attrs_ah =
	UVERBS_TYPE_ALLOC_IDR(0, uverbs_free_ah);

const struct uverbs_obj_idr_type uverbs_type_attrs_flow =
	UVERBS_TYPE_ALLOC_IDR(0, uverbs_free_flow);

const struct uverbs_obj_idr_type uverbs_type_attrs_wq =
	UVERBS_TYPE_ALLOC_IDR_SZ(sizeof(struct ib_uwq_object), 0,
				 uverbs_free_wq);

const struct uverbs_obj_idr_type uverbs_type_attrs_rwq_ind_table =
	UVERBS_TYPE_ALLOC_IDR(0, uverbs_free_rwq_ind_tbl);

const struct uverbs_obj_idr_type uverbs_type_attrs_xrcd =
	UVERBS_TYPE_ALLOC_IDR_SZ(sizeof(struct ib_uxrcd_object), 0,
				 uverbs_free_xrcd);

const struct uverbs_obj_idr_type uverbs_type_attrs_pd =
	/* 2 is used in order to free the PD after MRs */
	UVERBS_TYPE_ALLOC_IDR(2, uverbs_free_pd);
