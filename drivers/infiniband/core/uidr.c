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
#include <rdma/uverbs_ioctl.h>
#include "uidr.h"
#include "uobject.h"

struct uverbs_uobject_type *uverbs_get_type(struct ib_device *ibdev,
					    uint16_t type)
{
	struct uverbs_uobject_type *uobj_type;

	list_for_each_entry(uobj_type, &ibdev->type_list, type_list) {
		if (uobj_type->obj_type == type)
			return uobj_type;
	}

	return NULL;
}

static int uverbs_lock_object(struct ib_uobject *uobj, int access)
{
	if (access == UVERBS_IDR_ACCESS_READ)
		return __atomic_add_unless(&uobj->usecnt, 1, -1) == -1 ?
			-EBUSY : 0;
	else
		/* lock is either WRITE or DESTROY - should be exclusive */
		return atomic_cmpxchg(&uobj->usecnt, 0, -1) == 0 ? 0 : -EBUSY;
}

static struct ib_uobject *get_uobject_from_context(struct ib_ucontext *ucontext,
						   const struct uverbs_uobject_type *type,
						   uint32_t idr)
{
	struct uverbs_uobject_list *iter;
	struct ib_uobject *uobj;

	/* TODO: use something smarter.... hash? */
	list_for_each_entry(iter, &ucontext->uobjects_lists, type_list)
		if (iter->type == type)
			list_for_each_entry(uobj, &iter->list, idr_list)
				if (uobj->id == idr)
					return uobj;

	return NULL;
}

struct ib_uobject *uverbs_get_type_from_idr(struct uverbs_uobject_type *type,
					    struct ib_ucontext *ucontext,
					    int access,
					    uint32_t idr)
{
	struct ib_uobject *uobj;
	int ret;

	if (access == UVERBS_IDR_ACCESS_NEW) {
		uobj = kmalloc(sizeof(*uobj), GFP_KERNEL);
		if (!uobj)
			return ERR_PTR(-ENOMEM);

		init_uobj(uobj, 0, ucontext);

		/* lock idr */
		ret = ib_uverbs_uobject_add(uobj, type);
		if (ret)
			goto free_uobj;

		ret = uverbs_lock_object(uobj, access);
		if (ret)
			goto remove_uobj;
	} else {
		uobj = get_uobject_from_context(ucontext, type, idr);

		if (uobj) {
			ret = uverbs_lock_object(uobj, access);
			if (ret)
				return ERR_PTR(ret);
			return uobj;
		}

		return ERR_PTR(-ENOENT);
	}
remove_uobj:
	ib_uverbs_uobject_remove(uobj);
free_uobj:
	kfree(uobj);
	return ERR_PTR(ret);
}

static void uverbs_unlock_object(struct ib_uobject *uobj, int access)
{
	if (access == UVERBS_IDR_ACCESS_READ) {
		atomic_dec(&uobj->usecnt);
	} else {
		if (access == UVERBS_IDR_ACCESS_NEW)
			ib_uverbs_uobject_enable(uobj);

		if (access == UVERBS_IDR_ACCESS_WRITE ||
		    access == UVERBS_IDR_ACCESS_NEW)
			atomic_set(&uobj->usecnt, 0);

		if (access == UVERBS_IDR_ACCESS_DESTROY)
			ib_uverbs_uobject_remove(uobj);
	}
}

void uverbs_unlock_objects(struct uverbs_attr_array *attr_array,
			   size_t num,
			   const struct action_spec *chain,
			   bool success)
{
	unsigned int i;

	for (i = 0; i < num; i++) {
		struct uverbs_attr_array *attr_spec_array = &attr_array[i];
		const struct uverbs_attr_chain_spec *chain_spec =
			chain->validator_chains[i];
		unsigned int j;

		for (j = 0; j < attr_spec_array->num_attrs; j++) {
			struct uverbs_attr *attr = &attr_spec_array->attrs[j];
			struct uverbs_attr_spec *spec = &chain_spec->attrs[j];

			if (spec->type != UVERBS_ATTR_TYPE_IDR || !attr->valid)
				continue;

			/* TODO: if (!success) -> reduce refcount, otherwise
			 * fetching from idr already increased the refcount
			 */
			uverbs_unlock_object(attr->obj_attr.uobject,
					     spec->idr.access);
		}
	}
}

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

	if (!uobj->live)
		return NULL;

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

