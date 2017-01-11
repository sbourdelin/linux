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

bool uverbs_is_live(struct ib_uobject *uobj)
{
	return uobj == get_uobj_rcu(uobj->id, uobj->context);
}

struct ib_ucontext_lock {
	struct kref  ref;
	/* locking the uobjects_list */
	struct mutex lock;
};

static void init_uobjects_list_lock(struct ib_ucontext_lock *lock)
{
	mutex_init(&lock->lock);
	kref_init(&lock->ref);
}

static void release_uobjects_list_lock(struct kref *ref)
{
	struct ib_ucontext_lock *lock = container_of(ref,
						     struct ib_ucontext_lock,
						     ref);

	kfree(lock);
}

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

static struct ib_uobject *uverbs_priv_fd_to_uobject(void *priv)
{
	return priv - sizeof(struct ib_uobject);
}

static struct ib_uobject *uverbs_get_uobject_from_fd(const struct uverbs_type_alloc_action *type_alloc,
						     struct ib_ucontext *ucontext,
						     enum uverbs_idr_access access,
						     unsigned int fd)
{
	if (access == UVERBS_ACCESS_NEW) {
		int _fd;
		struct ib_uobject *uobj = NULL;
		struct file *filp;

		_fd = get_unused_fd_flags(O_CLOEXEC);
		if (_fd < 0)
			return ERR_PTR(_fd);

		uobj = kmalloc(type_alloc->obj_size, GFP_KERNEL);
		if (!uobj) {
			put_unused_fd(_fd);
			return ERR_PTR(-ENOMEM);
		}

		init_uobj(uobj, ucontext);
		filp = anon_inode_getfile(type_alloc->fd.name,
					  type_alloc->fd.fops,
					  uverbs_fd_uobj_to_priv(uobj),
					  type_alloc->fd.flags);
		if (IS_ERR(filp)) {
			put_unused_fd(_fd);
			kfree(uobj);
			return (void *)filp;
		}

		/*
		 * user_handle should be filled by the user,
		 * the list is filled in the commit operation.
		 */
		uobj->type = type_alloc;
		uobj->id = _fd;
		uobj->object = filp;

		return uobj;
	} else if (access == UVERBS_ACCESS_READ) {
		struct file *f = fget(fd);
		struct ib_uobject *uobject;

		if (!f)
			return ERR_PTR(-EBADF);

		uobject = uverbs_priv_fd_to_uobject(f->private_data);
		if (f->f_op != type_alloc->fd.fops ||
		    !uobject->context) {
			fput(f);
			return ERR_PTR(-EBADF);
		}

		/*
		 * No need to protect it with a ref count, as fget increases
		 * f_count.
		 */
		return uobject;
	} else {
		return ERR_PTR(-EOPNOTSUPP);
	}
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
		return uverbs_get_uobject_from_fd(type_alloc, ucontext, access,
						  id);
}

static void uverbs_uobject_add(struct ib_uobject *uobject)
{
	mutex_lock(&uobject->context->uobjects_lock->lock);
	list_add(&uobject->list, &uobject->context->uobjects);
	mutex_unlock(&uobject->context->uobjects_lock->lock);
}

static void uverbs_uobject_remove(struct ib_uobject *uobject, bool lock)
{
	/*
	 * Calling remove requires exclusive access, so it's not possible
	 * another thread will use our object since the function is called
	 * with exclusive access.
	 */
	uverbs_idr_remove_uobj(uobject);
	if (lock)
		mutex_lock(&uobject->context->uobjects_lock->lock);
	list_del(&uobject->list);
	if (lock)
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
			uverbs_uobject_remove(uobj, true);
		else
			up_write(&uobj->usecnt);
		break;
	}
}

static void uverbs_finalize_fd(struct ib_uobject *uobj,
			       enum uverbs_idr_access access,
			       bool commit)
{
	struct file *filp = uobj->object;

	if (access == UVERBS_ACCESS_NEW) {
		if (commit) {
			uobj->uobjects_lock = uobj->context->uobjects_lock;
			kref_get(&uobj->uobjects_lock->ref);
			uverbs_uobject_add(uobj);
			fd_install(uobj->id, uobj->object);
		} else {
			/* Unsuccessful NEW */
			fput(filp);
			put_unused_fd(uobj->id);
			kfree(uobj);
		}
	} else {
		fput(filp);
	}
}

void uverbs_finalize_object(struct ib_uobject *uobj,
			    enum uverbs_idr_access access,
			    bool commit)
{
	if (uobj->type->type == UVERBS_ATTR_TYPE_IDR)
		uverbs_finalize_idr(uobj, access, commit);
	else if (uobj->type->type == UVERBS_ATTR_TYPE_FD)
		uverbs_finalize_fd(uobj, access, commit);
	else
		WARN_ON(true);
}

static void uverbs_remove_fd(struct ib_uobject *uobject)
{
	if (uobject->context) {
		list_del(&uobject->list);
		uobject->context = NULL;
	}
}

/* user should release the uobject in the release file_operation callback. */
void uverbs_close_fd(struct file *f)
{
	struct ib_uobject *uobject = uverbs_priv_fd_to_uobject(f->private_data);

	mutex_lock(&uobject->uobjects_lock->lock);
	uverbs_remove_fd(uobject);
	mutex_unlock(&uobject->uobjects_lock->lock);
	kref_put(&uobject->uobjects_lock->ref, release_uobjects_list_lock);
}

void uverbs_cleanup_fd(void *private_data)
{
	struct ib_uobject *uobject = uverbs_priv_fd_to_uobject(private_data);

	kfree(uobject);
}

static unsigned int get_max_type_orders(const struct uverbs_root *root)
{
	unsigned int i;
	unsigned int max = 0;

	for (i = 0; i < root->num_groups; i++) {
		unsigned int j;
		const struct uverbs_type_group *types = root->type_groups[i];

		for (j = 0; j < types->num_types; j++) {
			/*
			 * Either this type isn't supported by this ib_device
			 * (as the group is an array of pointers to types
			 * indexed by the type) or this type is supported, but
			 * we can't instantiate objects from this type
			 * (e.g. you can't instantiate objects of
			 * UVERBS_DEVICE).
			 */
			if (!types->types[j] || !types->types[j]->alloc)
				continue;
			if (types->types[j]->alloc->order > max)
				max = types->types[j]->alloc->order;
		}
	}

	return max;
}

void uverbs_release_ucontext(struct ib_ucontext *ucontext)
{
	/*
	 * Since FD objects could outlive their context, we use a kref'ed
	 * lock. This lock is referenced when a context and FD objects are
	 * created. This lock protects concurrent context release from FD
	 * objects release. Therefore, we need to put this lock object in
	 * the context and every FD object release.
	 */
	kref_put(&ucontext->uobjects_lock->ref, release_uobjects_list_lock);
}

void uverbs_cleanup_ucontext(struct ib_ucontext *ucontext,
			     const struct uverbs_root *root)
{
	unsigned int num_orders = get_max_type_orders(root);
	unsigned int i;

	for (i = 0; i <= num_orders; i++) {
		struct ib_uobject *obj, *next_obj;

		/*
		 * This souldn't run while executing other commands on this
		 * context. Thus, the only thing we should take care of is
		 * releasing a FD while traversing this list. The FD could be
		 * closed and released from the _release fop of this FD.
		 * In order to mitigate this, we add a lock.
		 * We take and release the lock per order traversal in order
		 * to let other threads (which might still use the FDs) chance
		 * to run.
		 */
		mutex_lock(&ucontext->uobjects_lock->lock);
		list_for_each_entry_safe(obj, next_obj, &ucontext->uobjects,
					 list)
			if (obj->type->order == i) {
				obj->type->free_fn(obj->type, obj);
				if (obj->type->type == UVERBS_ATTR_TYPE_IDR)
					uverbs_uobject_remove(obj, false);
				else
					uverbs_remove_fd(obj);
			}
		mutex_unlock(&ucontext->uobjects_lock->lock);
	}
	uverbs_release_ucontext(ucontext);
}

int uverbs_initialize_ucontext(struct ib_ucontext *ucontext)
{
	ucontext->uobjects_lock = kmalloc(sizeof(*ucontext->uobjects_lock),
					  GFP_KERNEL);
	if (!ucontext->uobjects_lock)
		return -ENOMEM;

	init_uobjects_list_lock(ucontext->uobjects_lock);
	INIT_LIST_HEAD(&ucontext->uobjects);

	return 0;
}

