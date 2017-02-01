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

void uverbs_uobject_get(struct ib_uobject *uobj)
{
	kref_get(&uobj->ref);
}

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

static struct ib_uobject *alloc_begin_fd_uobject(const struct uverbs_obj_type *type,
						 struct ib_ucontext *ucontext)
{
	const struct uverbs_obj_fd_type *fd_type =
		container_of(type, struct uverbs_obj_fd_type, type);
	int new_fd;
	struct ib_uobject_file *uobj_file = NULL;
	struct file *filp;

	new_fd = get_unused_fd_flags(O_CLOEXEC);
	if (new_fd < 0)
		return ERR_PTR(new_fd);

	uobj_file = kmalloc(fd_type->obj_size, GFP_KERNEL);
	if (!uobj_file) {
		put_unused_fd(new_fd);
		return ERR_PTR(-ENOMEM);
	}

	filp = anon_inode_getfile(fd_type->name,
				  fd_type->fops,
				  uobj_file,
				  fd_type->flags);
	if (IS_ERR(filp)) {
		put_unused_fd(new_fd);
		kfree(uobj_file);
		return (void *)filp;
	}

	init_uobj(&uobj_file->uobj, ucontext, type);
	uobj_file->uobj.id = new_fd;
	uobj_file->uobj.object = filp;
	uobj_file->ufile = ucontext->ufile;

	return &uobj_file->uobj;
}

static struct ib_uobject *lookup_get_fd_uobject(const struct uverbs_obj_type *type,
						struct ib_ucontext *ucontext,
						int id, bool write)
{
	struct file *f;
	struct ib_uobject *uobject;
	const struct uverbs_obj_fd_type *fd_type =
		container_of(type, struct uverbs_obj_fd_type, type);

	if (write)
		return ERR_PTR(-EOPNOTSUPP);

	f = fget(id);
	if (!f)
		return ERR_PTR(-EBADF);

	uobject = f->private_data;
	if (f->f_op != fd_type->fops ||
	    !uobject->context) {
		fput(f);
		return ERR_PTR(-EBADF);
	}

	/*
	 * No need to protect it with a ref count, as fget increases
	 * f_count.
	 */
	return uobject;
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

static void lookup_put_fd_uobject(struct ib_uobject *uobj, bool write)
{
	struct file *filp = uobj->object;

	WARN_ON(write);
	fput(filp);
}

static void alloc_commit_fd_uobject(struct ib_uobject *uobj)
{
	struct ib_uobject_file *uobj_file =
		container_of(uobj, struct ib_uobject_file, uobj);

	kref_get(&uobj_file->ufile->ref);
	uverbs_uobject_add(&uobj_file->uobj);
	fd_install(uobj_file->uobj.id, uobj->object);
	/* This shouldn't be used anymore. Use the file object instead */
	uobj_file->uobj.id = 0;
}

static void alloc_abort_fd_uobject(struct ib_uobject *uobj)
{
	struct ib_uobject_file *uobj_file =
		container_of(uobj, struct ib_uobject_file, uobj);
	struct file *filp = uobj->object;

	/* Unsuccessful NEW */
	fput(filp);
	put_unused_fd(uobj_file->uobj.id);
	uverbs_uobject_put(&uobj_file->uobj);
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

static void release_fd_uobject(struct ib_uobject *uobj)
{
	kfree(container_of(uobj, struct ib_uobject_file, uobj));
}

static void destroy_commit_null_uobject(struct ib_uobject *uobj)
{
	WARN_ON(true);
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
		 * context. Thus, the only thing we should take care of is
		 * releasing a FD while traversing this list. The FD could be
		 * closed and released from the _release fop of this FD.
		 * In order to mitigate this, we add a lock.
		 * We take and release the lock per order traversal in order
		 * to let other threads (which might still use the FDs) chance
		 * to run.
		 */
		mutex_lock(&ucontext->lock);
		list_for_each_entry_safe(obj, next_obj, &ucontext->uobjects,
					 list)
			if (obj->type->destroy_order == cur_order) {
				list_del(&obj->list);
				obj->type->ops->hot_unplug(obj, device_removed);
			} else {
				next_order = min(next_order,
						 obj->type->destroy_order);
			}
		mutex_unlock(&ucontext->lock);
		cur_order = next_order;
	}
}

void uverbs_initialize_ucontext(struct ib_ucontext *ucontext)
{
	mutex_init(&ucontext->lock);
	INIT_LIST_HEAD(&ucontext->uobjects);
}

static void hot_unplug_fd_uobject(struct ib_uobject *uobj, bool device_removed)
{
	const struct uverbs_obj_fd_type *fd_type =
		container_of(uobj->type, struct uverbs_obj_fd_type, type);
	struct ib_uobject_file *uobj_file =
		container_of(uobj, struct ib_uobject_file, uobj);

	fd_type->hot_unplug(uobj_file, device_removed);
	uobj_file->uobj.context = NULL;
}

const struct uverbs_obj_type_ops uverbs_fd_ops = {
	.alloc_begin = alloc_begin_fd_uobject,
	.lookup_get = lookup_get_fd_uobject,
	.alloc_commit = alloc_commit_fd_uobject,
	.alloc_abort = alloc_abort_fd_uobject,
	.lookup_put = lookup_put_fd_uobject,
	.destroy_commit = destroy_commit_null_uobject,
	.hot_unplug = hot_unplug_fd_uobject,
	.release = release_fd_uobject,
};

void uverbs_close_fd(struct file *f)
{
	struct ib_uobject_file *uobj_file = f->private_data;

	mutex_lock(&uobj_file->ufile->cleanup_mutex);
	if (uobj_file->uobj.context) {
		mutex_lock(&uobj_file->uobj.context->lock);
		list_del(&uobj_file->uobj.list);
		mutex_unlock(&uobj_file->uobj.context->lock);
		uobj_file->uobj.context = NULL;
	}
	mutex_unlock(&uobj_file->ufile->cleanup_mutex);
	kref_put(&uobj_file->ufile->ref, ib_uverbs_release_file);
	uverbs_uobject_put(&uobj_file->uobj);
}

