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

#include <linux/file.h>
#include <linux/anon_inodes.h>
#include <rdma/ib_verbs.h>
#include <rdma/uverbs_types.h>
#include "uverbs.h"
#include "rdma_core.h"

static void uverbs_uobject_put_ref(struct kref *ref)
{
	struct ib_uobject *uobj =
		container_of(ref, struct ib_uobject, ref);

	uobj->type->ops->release(uobj);
}

void uverbs_uobject_put(struct ib_uobject *uobj)
{
	kref_put(&uobj->ref, uverbs_uobject_put_ref);
}

static int uverbs_lock_object(struct ib_uobject *uobj, bool write)
{
	if (!write)
		return down_read_trylock(&uobj->currently_used) == 1 ? 0 :
			-EBUSY;

	/* lock is either WRITE or DESTROY - should be exclusive */
	return down_write_trylock(&uobj->currently_used) == 1 ? 0 : -EBUSY;
}

static void init_uobj(struct ib_uobject *uobj, struct ib_ucontext *context,
		      const struct uverbs_obj_type *type)
{
	/*
	 * user_handle should be filled by the handler,
	 * The object is added to the list in the commit stage.
	 */
	init_rwsem(&uobj->currently_used);
	uobj->context     = context;
	uobj->type	  = type;
	kref_init(&uobj->ref);
}

static int idr_add_uobj(struct ib_uobject *uobj)
{
	int ret;

	idr_preload(GFP_KERNEL);
	spin_lock(&uobj->context->ufile->idr_lock);

	/*
	 * We start with allocating an idr pointing to NULL. This represents an
	 * object which isn't initialized yet. We'll replace it later on with
	 * the real object once we commit.
	 */
	ret = idr_alloc(&uobj->context->ufile->idr, NULL, 0,
			min_t(unsigned long, U32_MAX - 1, INT_MAX), GFP_NOWAIT);
	if (ret >= 0)
		uobj->id = ret;

	spin_unlock(&uobj->context->ufile->idr_lock);
	idr_preload_end();

	return ret < 0 ? ret : 0;
}

static void uverbs_idr_remove_uobj(struct ib_uobject *uobj)
{
	spin_lock(&uobj->context->ufile->idr_lock);
	idr_remove(&uobj->context->ufile->idr, uobj->id);
	spin_unlock(&uobj->context->ufile->idr_lock);
}

/* Returns the ib_uobject or an error. The caller should check for IS_ERR. */
static struct ib_uobject *lookup_get_idr_uobject(const struct uverbs_obj_type *type,
						 struct ib_ucontext *ucontext,
						 int id, bool write)
{
	struct ib_uobject *uobj;
	int ret;

	rcu_read_lock();
	/* object won't be released as we're protected in rcu */
	uobj = idr_find(&ucontext->ufile->idr, id);
	if (!uobj) {
		uobj = ERR_PTR(-ENOENT);
		goto free;
	}

	if (uobj->type != type) {
		uobj = ERR_PTR(-EINVAL);
		goto free;
	}

	ret = uverbs_lock_object(uobj, write);
	if (ret)
		uobj = ERR_PTR(ret);
free:
	rcu_read_unlock();
	return uobj;
}

static struct ib_uobject *alloc_begin_idr_uobject(const struct uverbs_obj_type *type,
						  struct ib_ucontext *ucontext)
{
	int ret;
	const struct uverbs_obj_idr_type *idr_type =
		container_of(type, struct uverbs_obj_idr_type, type);
	struct ib_uobject *uobj = kmalloc(idr_type->obj_size, GFP_KERNEL);

	if (!uobj)
		return ERR_PTR(-ENOMEM);

	init_uobj(uobj, ucontext, type);
	ret = idr_add_uobj(uobj);
	if (ret) {
		kfree(uobj);
		return ERR_PTR(ret);
	}

	return uobj;
}

static void uverbs_uobject_add(struct ib_uobject *uobject)
{
	mutex_lock(&uobject->context->lock);
	list_add(&uobject->list, &uobject->context->uobjects);
	mutex_unlock(&uobject->context->lock);
}

static void alloc_commit_idr_uobject(struct ib_uobject *uobj)
{
	uverbs_uobject_add(uobj);
	spin_lock(&uobj->context->ufile->idr_lock);
	/*
	 * We already allocated this IDR with a NULL object, so
	 * this shouldn't fail.
	 */
	WARN_ON(idr_replace(&uobj->context->ufile->idr,
			    uobj, uobj->id));
	spin_unlock(&uobj->context->ufile->idr_lock);
}

static void _put_uobj_ref(struct kref *ref)
{
	kfree(container_of(ref, struct ib_uobject, ref));
}

static void alloc_abort_idr_uobject(struct ib_uobject *uobj)
{
	uverbs_idr_remove_uobj(uobj);
	/*
	 * we don't need kfree_rcu here, as the uobject wasn't exposed to any
	 * other verb.
	 */
	kref_put(&uobj->ref, _put_uobj_ref);
}

static void lookup_put_idr_uobject(struct ib_uobject *uobj, bool write)
{
	if (write)
		up_write(&uobj->currently_used);
	else
		up_read(&uobj->currently_used);
}

static void destroy_commit_idr_uobject(struct ib_uobject *uobj)
{
	uverbs_idr_remove_uobj(uobj);
	mutex_lock(&uobj->context->lock);
	list_del(&uobj->list);
	mutex_unlock(&uobj->context->lock);
	uverbs_uobject_put(uobj);
}

static void hot_unplug_idr_uobject(struct ib_uobject *uobj, bool device_removed)
{
	const struct uverbs_obj_idr_type *idr_type =
		container_of(uobj->type, struct uverbs_obj_idr_type, type);

	idr_type->hot_unplug(uobj);
	uverbs_idr_remove_uobj(uobj);
	uverbs_uobject_put(uobj);
}

static void release_idr_uobject(struct ib_uobject *uobj)
{
	/*
	 * When we destroy an object, we first just lock it for WRITE and
	 * actually DESTROY it in the finalize stage. So, the problematic
	 * scenario is when we just started the finalize stage of the
	 * destruction (nothing was executed yet). Now, the other thread
	 * fetched the object for READ access, but it didn't lock it yet.
	 * The DESTROY thread continues and starts destroying the object.
	 * When the other thread continue - without the RCU, it would
	 * access freed memory. However, the rcu_read_lock delays the free
	 * until the rcu_read_lock of the READ operation quits. Since the
	 * write lock of the object is still taken by the DESTROY flow, the
	 * READ operation will get -EBUSY and it'll just bail out.
	 */
	kfree_rcu(uobj, rcu);
}

const struct uverbs_obj_type_ops uverbs_idr_ops = {
	.alloc_begin = alloc_begin_idr_uobject,
	.lookup_get = lookup_get_idr_uobject,
	.alloc_commit = alloc_commit_idr_uobject,
	.alloc_abort = alloc_abort_idr_uobject,
	.lookup_put = lookup_put_idr_uobject,
	.destroy_commit = destroy_commit_idr_uobject,
	.hot_unplug = hot_unplug_idr_uobject,
	.release = release_idr_uobject,
};

void uverbs_cleanup_ucontext(struct ib_ucontext *ucontext, bool device_removed)
{
	unsigned int cur_order = 0;

	while (!list_empty(&ucontext->uobjects)) {
		struct ib_uobject *obj, *next_obj;
		unsigned int next_order = UINT_MAX;

		/*
		 * This shouldn't run while executing other commands on this
		 * context, thus no lock is required.
		 */
		list_for_each_entry_safe(obj, next_obj, &ucontext->uobjects,
					 list)
			if (obj->type->destroy_order == cur_order) {
				list_del(&obj->list);
				obj->type->ops->hot_unplug(obj, device_removed);
			} else {
				next_order = min(next_order,
						 obj->type->destroy_order);
			}
		cur_order = next_order;
	}
}

void uverbs_initialize_ucontext(struct ib_ucontext *ucontext)
{
	mutex_init(&ucontext->lock);
	INIT_LIST_HEAD(&ucontext->uobjects);
}

