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

/*
 * lockless - the list shouldn't change. If disassociate is carrie out during
 * this, we'll wait until all current executing commands are finished.
 */
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

static int uverbs_lock_object(struct ib_uobject *uobj,
			      enum uverbs_idr_access access)
{
	if (access == UVERBS_IDR_ACCESS_READ)
		return __atomic_add_unless(&uobj->usecnt, 1, -1) == -1 ?
			-EBUSY : 0;
	else
		/* lock is either WRITE or DESTROY - should be exclusive */
		return atomic_cmpxchg(&uobj->usecnt, 0, -1) == 0 ? 0 : -EBUSY;
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
						   const struct uverbs_uobject_type *type,
						   u32 idr,
						   enum uverbs_idr_access access)
{
	struct ib_uobject *uobj;
	int ret;

	rcu_read_lock();
	uobj = get_uobj(idr, ucontext);
	if (!uobj)
		goto free;

	if (uobj->type->type != type) {
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

struct ib_uobject *uverbs_get_type_from_idr(struct uverbs_uobject_type *type,
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

static void uverbs_unlock_object(struct ib_uobject *uobj,
				 enum uverbs_idr_access access,
				 bool success)
{
	switch (access) {
	case UVERBS_IDR_ACCESS_READ:
		atomic_dec(&uobj->usecnt);
		break;
	case UVERBS_IDR_ACCESS_NEW:
		if (success) {
			atomic_set(&uobj->usecnt, 0);
			ib_uverbs_uobject_enable(uobj);
		} else {
			remove_uobj(uobj);
			put_uobj(uobj);
		}
		break;
	case UVERBS_IDR_ACCESS_WRITE:
		atomic_set(&uobj->usecnt, 0);
		break;
	case UVERBS_IDR_ACCESS_DESTROY:
		if (success)
			ib_uverbs_uobject_remove(uobj);
		else
			atomic_set(&uobj->usecnt, 0);
		break;
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

			/*
			 * refcounts should be handled at the object level and
			 * not at the uobject level.
			 */
			uverbs_unlock_object(attr->obj_attr.uobject,
					     spec->idr.access, success);
		}
	}
}

int ib_uverbs_uobject_type_add(struct list_head	*head,
			       void (*free)(struct uverbs_uobject_type *uobject_type,
					    struct ib_uobject *uobject,
					    struct ib_ucontext *ucontext),
			       uint16_t	obj_type)
{
	/*
	 * Allocate a new object type for the vendor, this should be done when a
	 * vendor is initialized.
	 */
	struct uverbs_uobject_type *uobject_type;

	uobject_type = kzalloc(sizeof(*uobject_type), GFP_KERNEL);
	if (!uobject_type)
		return -ENOMEM;

	uobject_type->free = free;
	uobject_type->obj_type = obj_type;
	list_add_tail(&uobject_type->type_list, head);
	return 0;
}
EXPORT_SYMBOL(ib_uverbs_uobject_type_add);

/* Should only be called when device is destroyed (remove_one?) */
static void ib_uverbs_uobject_type_remove(struct uverbs_uobject_type *uobject_type)
{
	/*
	 * Allocate a new object type for the vendor, this should be done when a
	 * vendor is initialized.
	 */
	WARN_ON(list_empty(&uobject_type->type_list));
	list_del(&uobject_type->type_list);
	kfree(uobject_type);
}
EXPORT_SYMBOL(ib_uverbs_uobject_type_remove);

/*
 * Done when device is destroyed. No one should touch the list or use its
 * elements here.
 */
void ib_uverbs_uobject_types_remove(struct ib_device *ib_dev)
{
	struct uverbs_uobject_type *iter, *temp;

	list_for_each_entry_safe(iter, temp, &ib_dev->type_list, type_list)
		ib_uverbs_uobject_type_remove(iter);
}
EXPORT_SYMBOL(ib_uverbs_uobject_types_remove);

void ib_uverbs_uobject_type_cleanup_ucontext(struct ib_ucontext *ucontext)
{
	struct uverbs_uobject_list *uobject_list, *next_list;

	list_for_each_entry_safe(uobject_list, next_list,
				 &ucontext->uobjects_lists, type_list) {
		struct ib_uobject *obj, *next_obj;

		/*
		 * No need to take lock here, as cleanup should be called
		 * after all commands finished executing. Newly executed
		 * commands should fail.
		 */
		list_for_each_entry_safe(obj, next_obj, &uobject_list->list,
					 list)
			ib_uverbs_uobject_remove(obj);

		list_del(&uobject_list->type_list);
	}
}

int ib_uverbs_uobject_type_initialize_ucontext(struct ib_ucontext *ucontext,
					       struct list_head *type_list)
{
	/* create typed list in ucontext */
	struct uverbs_uobject_type *type;
	int err;

	INIT_LIST_HEAD(&ucontext->uobjects_lists);

	list_for_each_entry(type, type_list, type_list) {
		struct uverbs_uobject_list *cur;

		cur = kzalloc(sizeof(*cur), GFP_KERNEL);
		if (!cur) {
			err = -ENOMEM;
			goto err;
		}

		cur->type = type;
		INIT_LIST_HEAD(&cur->list);
		list_add_tail(&cur->type_list, &ucontext->uobjects_lists);
		mutex_init(&cur->uobj_lock);
	}

	return 0;

err:
	ib_uverbs_uobject_type_cleanup_ucontext(ucontext);
	return err;
}

int ib_uverbs_uobject_add(struct ib_uobject *uobject,
			  struct uverbs_uobject_type *uobject_type)
{
	int ret = -EINVAL;
	struct uverbs_uobject_list *type;

	/* No need for locking here is type list shouldn't be changed */
	list_for_each_entry(type, &uobject->context->uobjects_lists, type_list)
		if (type->type == uobject_type) {
			uobject->type = type;
			ret = add_uobj(uobject);
			return ret;
		}

	return ret;
}

void ib_uverbs_uobject_enable(struct ib_uobject *uobject)
{
	mutex_lock(&uobject->type->uobj_lock);
	list_add(&uobject->list, &uobject->type->list);
	mutex_unlock(&uobject->type->uobj_lock);
	uobject->live = 1;
}

void ib_uverbs_uobject_remove(struct ib_uobject *uobject)
{
	/*
	 * Calling remove requires exclusive access, so it's not possible
	 * another thread will use our object.
	 */
	uobject->live = 0;
	uobject->type->type->free(uobject->type->type, uobject,
				  uobject->context);
	mutex_lock(&uobject->type->uobj_lock);
	list_del(&uobject->list);
	mutex_unlock(&uobject->type->uobj_lock);
	remove_uobj(uobject);
	put_uobj(uobject);
}
