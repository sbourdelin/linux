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

#include <rdma/uverbs_ioctl_cmd.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/ib_verbs.h>
#include <linux/bug.h>
#include <linux/file.h>
#include "rdma_core.h"
#include "uverbs.h"

#define IB_UVERBS_VENDOR_FLAG	0x8000

int ib_uverbs_std_dist(__u16 *attr_id, void *priv)
{
	if (*attr_id & IB_UVERBS_VENDOR_FLAG) {
		*attr_id &= ~IB_UVERBS_VENDOR_FLAG;
		return 1;
	}
	return 0;
}
EXPORT_SYMBOL(ib_uverbs_std_dist);

int uverbs_action_std_handle(struct ib_device *ib_dev,
			     struct ib_uverbs_file *ufile,
			     struct uverbs_attr_array *ctx, size_t num,
			     void *_priv)
{
	struct uverbs_action_std_handler *priv = _priv;

	if (!ufile->ucontext)
		return -EINVAL;

	WARN_ON((num != 1) && (num != 2));
	return priv->handler(ib_dev, ufile->ucontext, &ctx[0],
			     (num == 2 ? &ctx[1] : NULL),
			     priv->priv);
}
EXPORT_SYMBOL(uverbs_action_std_handle);

int uverbs_action_std_ctx_handle(struct ib_device *ib_dev,
				 struct ib_uverbs_file *ufile,
				 struct uverbs_attr_array *ctx, size_t num,
				 void *_priv)
{
	struct uverbs_action_std_ctx_handler *priv = _priv;

	WARN_ON((num != 1) && (num != 2));
	return priv->handler(ib_dev, ufile, &ctx[0], (num == 2 ? &ctx[1] : NULL), priv->priv);
}
EXPORT_SYMBOL(uverbs_action_std_ctx_handle);

static void free_ah(struct uverbs_uobject_type *uobject_type,
		    struct ib_uobject *uobject,
		    struct ib_ucontext *ucontext)
{
	ib_destroy_ah((struct ib_ah *)uobject->object);
}

static void free_flow(struct uverbs_uobject_type *uobject_type,
		      struct ib_uobject *uobject,
		      struct ib_ucontext *ucontext)
{
	ib_destroy_flow((struct ib_flow *)uobject->object);
}

static void free_mw(struct uverbs_uobject_type *uobject_type,
		    struct ib_uobject *uobject,
		    struct ib_ucontext *ucontext)
{
	uverbs_dealloc_mw((struct ib_mw *)uobject->object);
}

static void free_qp(struct uverbs_uobject_type *uobject_type,
		    struct ib_uobject *uobject,
		    struct ib_ucontext *ucontext)
{
	struct ib_qp *qp = uobject->object;
	struct ib_uqp_object *uqp =
		container_of(uobject, struct ib_uqp_object, uevent.uobject);

	if (qp != qp->real_qp) {
		ib_close_qp(qp);
	} else {
		ib_uverbs_detach_umcast(qp, uqp);
		ib_destroy_qp(qp);
	}
	ib_uverbs_release_uevent(ucontext->ufile, &uqp->uevent);
}

static void free_srq(struct uverbs_uobject_type *uobject_type,
		     struct ib_uobject *uobject,
		     struct ib_ucontext *ucontext)
{
	struct ib_srq *srq = uobject->object;
	struct ib_uevent_object *uevent =
		container_of(uobject, struct ib_uevent_object, uobject);

	ib_destroy_srq(srq);
	ib_uverbs_release_uevent(ucontext->ufile, uevent);
}

static void free_cq(struct uverbs_uobject_type *uobject_type,
		    struct ib_uobject *uobject,
		    struct ib_ucontext *ucontext)
{
	struct ib_cq *cq = uobject->object;
	struct ib_uverbs_event_file *ev_file = cq->cq_context;
	struct ib_ucq_object *ucq =
		container_of(uobject, struct ib_ucq_object, uobject);

	ib_destroy_cq(cq);
	ib_uverbs_release_ucq(ucontext->ufile, ev_file, ucq);
}

static void free_mr(struct uverbs_uobject_type *uobject_type,
		    struct ib_uobject *uobject,
		    struct ib_ucontext *ucontext)
{
	ib_dereg_mr((struct ib_mr *)uobject);
}

static void free_xrcd(struct uverbs_uobject_type *uobject_type,
		      struct ib_uobject *uobject,
		      struct ib_ucontext *ucontext)
{
	struct ib_xrcd *xrcd = uobject->object;

	mutex_lock(&ucontext->ufile->device->xrcd_tree_mutex);
	ib_uverbs_dealloc_xrcd(ucontext->ufile->device, xrcd);
	mutex_unlock(&ucontext->ufile->device->xrcd_tree_mutex);
}

static void free_pd(struct uverbs_uobject_type *uobject_type,
		    struct ib_uobject *uobject,
		    struct ib_ucontext *ucontext)
{
	ib_dealloc_pd((struct ib_pd *)uobject);
}

int rdma_initialize_common_types(struct ib_device *ib_dev, unsigned int types)
{
	static const struct
	{
		enum uverbs_common_types type;
		void (*free)(struct uverbs_uobject_type *uobject_type,
			     struct ib_uobject *uobject,
			     struct ib_ucontext *ucontext);

	} common_types[] = { /* by release order */
		{.type = UVERBS_TYPE_AH,	.free = free_ah},
		{.type = UVERBS_TYPE_MW,	.free = free_mw},
		{.type = UVERBS_TYPE_FLOW,	.free = free_flow},
		{.type = UVERBS_TYPE_QP,	.free = free_qp},
		{.type = UVERBS_TYPE_SRQ,	.free = free_srq},
		{.type = UVERBS_TYPE_CQ,	.free = free_cq},
		{.type = UVERBS_TYPE_MR,	.free = free_mr},
		{.type = UVERBS_TYPE_XRCD,	.free = free_xrcd},
		{.type = UVERBS_TYPE_PD,	.free = free_pd},
	};
	int ret = 0;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(common_types); i++) {
		if (types & common_types[i].type) {
			ret = ib_uverbs_uobject_type_add(&ib_dev->type_list,
							 common_types[i].free,
							 common_types[i].type);
			if (ret)
				goto free;
		}
	}

free:
	ib_uverbs_uobject_types_remove(ib_dev);
	return ret;
}
EXPORT_SYMBOL(rdma_initialize_common_types);

