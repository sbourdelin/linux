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
#include <rdma/uverbs_ioctl.h>
#include "uverbs.h"
#include "rdma_core.h"

static int uverbs_lock_object(struct ib_uobject *uobj,
			      enum uverbs_idr_access access)
{
	if (access == UVERBS_ACCESS_READ)
		return down_read_trylock(&uobj->usecnt) == 1 ? 0 : -EBUSY;

	/* lock is either WRITE or DESTROY - should be exclusive */
	return down_write_trylock(&uobj->usecnt) == 1 ? 0 : -EBUSY;
}

static struct ib_uobject *get_uobj_rcu(int id, struct ib_ucontext *context)
{
	struct ib_uobject *uobj;

	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "uverbs: get_uobj_rcu wasn't called in a rcu_read_lock()!");
	/* object won't be released as we're protected in rcu */
	uobj = idr_find(&context->device->idr, id);
	if (uobj) {
		if (uobj->context != context)
			uobj = NULL;
	}

	return uobj;
}

struct ib_ucontext_lock {
	/* locking the uobjects_list */
	struct mutex lock;
};

static void init_uobj(struct ib_uobject *uobj, struct ib_ucontext *context)
{
	init_rwsem(&uobj->usecnt);
	uobj->context     = context;
}

static int uverbs_idr_add_uobj(struct ib_uobject *uobj)
{
	int ret;

	idr_preload(GFP_KERNEL);
	spin_lock(&uobj->context->device->idr_lock);

	/*
	 * We start with allocating an idr pointing to NULL. This represents an
	 * object which isn't initialized yet. We'll replace it later on with
	 * the real object once we commit.
	 */
	ret = idr_alloc(&uobj->context->device->idr, NULL, 0, 0, GFP_NOWAIT);
	if (ret >= 0)
		uobj->id = ret;

	spin_unlock(&uobj->context->device->idr_lock);
	idr_preload_end();

	return ret < 0 ? ret : 0;
}

static void uverbs_idr_remove_uobj(struct ib_uobject *uobj)
{
	spin_lock(&uobj->context->device->idr_lock);
	idr_remove(&uobj->context->device->idr, uobj->id);
	spin_unlock(&uobj->context->device->idr_lock);
}

static void put_uobj(struct ib_uobject *uobj)
{
	/*
	 * When we destroy an object, we first just lock it for WRITE and
	 * actually DESTROY it in the finalize stage. So, the problematic
	 * scenario is when we just stared the finalize stage of the
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

/*
 * Returns the ib_uobject, NULL if the requested object isn't found or an error.
 * The caller should check for IS_ERR_OR_NULL.
 */
static struct ib_uobject *get_uobject_from_context(struct ib_ucontext *ucontext,
						   const struct uverbs_type_alloc_action *type,
						   u32 idr,
						   enum uverbs_idr_access access)
{
	struct ib_uobject *uobj;
	int ret;

	rcu_read_lock();
	uobj = get_uobj_rcu(idr, ucontext);
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
}

static struct ib_uobject *uverbs_get_uobject_from_idr(const struct uverbs_type_alloc_action *type_alloc,
						      struct ib_ucontext *ucontext,
						      enum uverbs_idr_access access,
						      uint32_t idr)
{
	struct ib_uobject *uobj;
	int ret;

	if (access == UVERBS_ACCESS_NEW) {
		uobj = kmalloc(type_alloc->obj_size, GFP_KERNEL);
		if (!uobj)
			return ERR_PTR(-ENOMEM);

		init_uobj(uobj, ucontext);

		uobj->type = type_alloc;
		ret = uverbs_idr_add_uobj(uobj);
		if (ret) {
			kfree(uobj);
			return ERR_PTR(ret);
		}

	} else {
		uobj = get_uobject_from_context(ucontext, type_alloc, idr,
						access);

		if (IS_ERR_OR_NULL(uobj))
			return ERR_PTR(-ENOENT);
	}

	return uobj;
}

struct ib_uobject *uverbs_get_uobject_from_context(const struct uverbs_type_alloc_action *type_alloc,
						   struct ib_ucontext *ucontext,
						   enum uverbs_idr_access access,
						   unsigned int id)
{
	if (type_alloc->type == UVERBS_ATTR_TYPE_IDR)
		return uverbs_get_uobject_from_idr(type_alloc, ucontext, access,
						   id);
	else
		return ERR_PTR(-ENOENT);
}

static void uverbs_uobject_add(struct ib_uobject *uobject)
{
	mutex_lock(&uobject->context->uobjects_lock->lock);
	list_add(&uobject->list, &uobject->context->uobjects);
	mutex_unlock(&uobject->context->uobjects_lock->lock);
}

static void uverbs_uobject_remove(struct ib_uobject *uobject)
{
	/*
	 * Calling remove requires exclusive access, so it's not possible
	 * another thread will use our object since the function is called
	 * with exclusive access.
	 */
	uverbs_idr_remove_uobj(uobject);
	mutex_lock(&uobject->context->uobjects_lock->lock);
	list_del(&uobject->list);
	mutex_unlock(&uobject->context->uobjects_lock->lock);
	put_uobj(uobject);
}

static void uverbs_finalize_idr(struct ib_uobject *uobj,
				enum uverbs_idr_access access,
				bool commit)
{
	switch (access) {
	case UVERBS_ACCESS_READ:
		up_read(&uobj->usecnt);
		break;
	case UVERBS_ACCESS_NEW:
		if (commit) {
			uverbs_uobject_add(uobj);
			spin_lock(&uobj->context->device->idr_lock);
			/*
			 * We already allocated this IDR with a NULL object, so
			 * this shouldn't fail.
			 */
			WARN_ON(idr_replace(&uobj->context->device->idr,
					    uobj, uobj->id));
			spin_unlock(&uobj->context->device->idr_lock);
		} else {
			uverbs_idr_remove_uobj(uobj);
			put_uobj(uobj);
		}
		break;
	case UVERBS_ACCESS_WRITE:
		up_write(&uobj->usecnt);
		break;
	case UVERBS_ACCESS_DESTROY:
		if (commit)
			uverbs_uobject_remove(uobj);
		else
			up_write(&uobj->usecnt);
		break;
	}
}

void uverbs_finalize_object(struct ib_uobject *uobj,
			    enum uverbs_idr_access access,
			    bool commit)
{
	if (uobj->type->type == UVERBS_ATTR_TYPE_IDR)
		uverbs_finalize_idr(uobj, access, commit);
	else
		WARN_ON(true);
}
