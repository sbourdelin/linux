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
#include "uverbs.h"
#include "rdma_core.h"
#include <rdma/uverbs_ioctl.h>

const struct uverbs_type *uverbs_get_type(const struct ib_device *ibdev,
					  uint16_t type)
{
	const struct uverbs_types_group *groups = ibdev->types_group;
	const struct uverbs_types *types;
	int ret = groups->dist(&type, groups->priv);

	if (ret >= groups->num_groups)
		return NULL;

	types = groups->type_groups[ret];

	if (type >= types->num_types)
		return NULL;

	return types->types[type];
}

static int uverbs_lock_object(struct ib_uobject *uobj,
			      enum uverbs_idr_access access)
{
	if (access == UVERBS_IDR_ACCESS_READ)
		return down_read_trylock(&uobj->usecnt) == 1 ? 0 : -EBUSY;

	/* lock is either WRITE or DESTROY - should be exclusive */
	return down_write_trylock(&uobj->usecnt) == 1 ? 0 : -EBUSY;
}

static struct ib_uobject *get_uobj(int id, struct ib_ucontext *context)
{
	struct ib_uobject *uobj;

	rcu_read_lock();
	uobj = idr_find(&context->device->idr, id);
	if (uobj && uobj->live) {
		if (uobj->context != context)
			uobj = NULL;
	}
	rcu_read_unlock();

	return uobj;
}

static void init_uobj(struct ib_uobject *uobj, u64 user_handle,
		      struct ib_ucontext *context)
{
	init_rwsem(&uobj->usecnt);
	uobj->user_handle = user_handle;
	uobj->context     = context;
	uobj->live        = 0;
}

static int add_uobj(struct ib_uobject *uobj)
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

static void remove_uobj(struct ib_uobject *uobj)
{
	spin_lock(&uobj->context->device->idr_lock);
	idr_remove(&uobj->context->device->idr, uobj->id);
	spin_unlock(&uobj->context->device->idr_lock);
}

static void put_uobj(struct ib_uobject *uobj)
{
	kfree_rcu(uobj, rcu);
}

static struct ib_uobject *get_uobject_from_context(struct ib_ucontext *ucontext,
						   const struct uverbs_type_alloc_action *type,
						   u32 idr,
						   enum uverbs_idr_access access)
{
	struct ib_uobject *uobj;
	int ret;

	rcu_read_lock();
	uobj = get_uobj(idr, ucontext);
	if (!uobj)
		goto free;

	if (uobj->type != type) {
		uobj = NULL;
		goto free;
	}

	ret = uverbs_lock_object(uobj, access);
	if (ret)
		uobj = ERR_PTR(ret);
free:
	rcu_read_unlock();
	return uobj;

	return NULL;
}

struct ib_uobject *uverbs_get_type_from_idr(const struct uverbs_type_alloc_action *type,
					    struct ib_ucontext *ucontext,
					    enum uverbs_idr_access access,
					    uint32_t idr)
{
	struct ib_uobject *uobj;
	int ret;

	if (access == UVERBS_IDR_ACCESS_NEW) {
		uobj = kmalloc(type->obj_size, GFP_KERNEL);
		if (!uobj)
			return ERR_PTR(-ENOMEM);

		init_uobj(uobj, 0, ucontext);

		/* lock idr */
		ret = ib_uverbs_uobject_add(uobj, type);
		if (ret) {
			kfree(uobj);
			return ERR_PTR(ret);
		}

	} else {
		uobj = get_uobject_from_context(ucontext, type, idr,
						access);

		if (!uobj)
			return ERR_PTR(-ENOENT);
	}

	return uobj;
}

void uverbs_unlock_object(struct ib_uobject *uobj,
			  enum uverbs_idr_access access,
			  bool success)
{
	switch (access) {
	case UVERBS_IDR_ACCESS_READ:
		up_read(&uobj->usecnt);
		break;
	case UVERBS_IDR_ACCESS_NEW:
		if (success) {
			ib_uverbs_uobject_enable(uobj);
		} else {
			remove_uobj(uobj);
			put_uobj(uobj);
		}
		break;
	case UVERBS_IDR_ACCESS_WRITE:
		up_write(&uobj->usecnt);
		break;
	case UVERBS_IDR_ACCESS_DESTROY:
		if (success)
			ib_uverbs_uobject_remove(uobj);
		else
			up_write(&uobj->usecnt);
		break;
	}
}

void uverbs_unlock_objects(struct uverbs_attr_array *attr_array,
			   size_t num,
			   const struct uverbs_action_spec *spec,
			   bool success)
{
	unsigned int i;

	for (i = 0; i < num; i++) {
		struct uverbs_attr_array *attr_spec_array = &attr_array[i];
		const struct uverbs_attr_group_spec *group_spec =
			spec->attr_groups[i];
		unsigned int j;

		for (j = 0; j < attr_spec_array->num_attrs; j++) {
			struct uverbs_attr *attr = &attr_spec_array->attrs[j];
			struct uverbs_attr_spec *spec = &group_spec->attrs[j];

			if (spec->type != UVERBS_ATTR_TYPE_IDR || !attr->valid)
				continue;

			/*
			 * refcounts should be handled at the object level and
			 * not at the uobject level.
			 */
			uverbs_unlock_object(attr->obj_attr.uobject,
					     spec->idr.access, success);
		}
	}
}

static unsigned int get_type_orders(const struct uverbs_types_group *types_group)
{
	unsigned int i;
	unsigned int max = 0;

	for (i = 0; i < types_group->num_groups; i++) {
		unsigned int j;
		const struct uverbs_types *types = types_group->type_groups[i];

		for (j = 0; j < types->num_types; j++) {
			if (!types->types[j] || !types->types[j]->alloc)
				continue;
			if (types->types[j]->alloc->order > max)
				max = types->types[j]->alloc->order;
		}
	}

	return max;
}

void ib_uverbs_uobject_type_cleanup_ucontext(struct ib_ucontext *ucontext,
					     const struct uverbs_types_group *types_group)
{
	unsigned int num_orders = get_type_orders(types_group);
	unsigned int i;

	for (i = 0; i <= num_orders; i++) {
		struct ib_uobject *obj, *next_obj;

		/*
		 * No need to take lock here, as cleanup should be called
		 * after all commands finished executing. Newly executed
		 * commands should fail.
		 */
		list_for_each_entry_safe(obj, next_obj, &ucontext->uobjects,
					 list)
			if (obj->type->order == i) {
				pr_info("deallocating object %p\n", obj);
				ib_uverbs_uobject_remove(obj);
			}
	}
}

void ib_uverbs_uobject_type_initialize_ucontext(struct ib_ucontext *ucontext)
{
	INIT_LIST_HEAD(&ucontext->uobjects);
	mutex_init(&ucontext->uobjects_lock);
}

int ib_uverbs_uobject_add(struct ib_uobject *uobject,
			  const struct uverbs_type_alloc_action *uobject_type)
{
	uobject->type = uobject_type;
	return add_uobj(uobject);
}

void ib_uverbs_uobject_enable(struct ib_uobject *uobject)
{
	mutex_lock(&uobject->context->uobjects_lock);
	list_add(&uobject->list, &uobject->context->uobjects);
	mutex_unlock(&uobject->context->uobjects_lock);
	uobject->live = 1;
}

void ib_uverbs_uobject_remove(struct ib_uobject *uobject)
{
	/*
	 * Calling remove requires exclusive access, so it's not possible
	 * another thread will use our object.
	 */
	uobject->live = 0;
	uobject->type->free_fn(uobject->type, uobject);
	mutex_lock(&uobject->context->uobjects_lock);
	list_del(&uobject->list);
	mutex_unlock(&uobject->context->uobjects_lock);
	remove_uobj(uobject);
	put_uobj(uobject);
}
