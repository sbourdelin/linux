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

#ifndef _UVERBS_TYPES_
#define _UVERBS_TYPES_

#include <linux/kernel.h>
#include <rdma/ib_verbs.h>

struct uverbs_obj_type;

struct uverbs_obj_type_ops {
	/*
	 * Get an ib_uobject that corresponds to the given id from ucontext,
	 * These functions could create or destroy objects if required.
	 * The action will be finalized only when commit, abort or put fops are
	 * called.
	 * The flow of the different actions is:
	 * [alloc]:	 Starts with alloc_begin. The handlers logic is than
	 *		 executed. If the handler is successful, alloc_commit
	 *		 is called and the object is inserted to the repository.
	 *		 Otherwise, alloc_abort is called and the object is
	 *		 destroyed.
	 * [lookup]:	 Starts with lookup_get which fetches and locks the
	 *		 object. After the handler finished using the object, it
	 *		 needs to call lookup_put to unlock it. The write flag
	 *		 indicates if the object is locked for exclusive access.
	 * [destroy]:	 Starts with lookup_get with write flag set. This locks
	 *		 the object for exclusive access. If the handler code
	 *		 completed successfully, destroy_commit is called and
	 *		 the ib_uobject is removed from the context's uobjects
	 *		 repository and put. Otherwise, lookup_put should be
	 *		 called.
	 * [hot_unplug]: Used when the context is destroyed (process
	 *		 termination, reset flow).
	 * [release]:    Releases the memory of the ib_uobject. This is called
	 *		 when the last reference is put. We favored a callback
	 *		 here as this might require tricks like kfree_rcu.
	 *		 This shouldn't be called explicitly by the user as it's
	 *		 used by uverbs_uobject_put.
	 */
	struct ib_uobject *(*alloc_begin)(const struct uverbs_obj_type *type,
					  struct ib_ucontext *ucontext);
	void (*alloc_commit)(struct ib_uobject *uobj);
	void (*alloc_abort)(struct ib_uobject *uobj);

	struct ib_uobject *(*lookup_get)(const struct uverbs_obj_type *type,
					 struct ib_ucontext *ucontext, int id,
					 bool write);
	void (*lookup_put)(struct ib_uobject *uobj, bool write);
	void (*destroy_commit)(struct ib_uobject *uobj);

	void (*hot_unplug)(struct ib_uobject *uobj, bool device_removed);

	void (*release)(struct ib_uobject *uobj);
};

struct uverbs_obj_type {
	const struct uverbs_obj_type_ops * const ops;
	unsigned int destroy_order;
};

struct uverbs_obj_idr_type {
	/*
	 * In idr based objects, uverbs_obj_type_ops points to a generic
	 * idr operations. In order to specialize the underlying types (e.g. CQ,
	 * QPs, etc.), we add obj_size and hot_unplug specific callbacks here.
	 */
	struct uverbs_obj_type  type;
	size_t			obj_size;
	/* The hot_unplug in ops initialized by idr, calls this callback */
	void (*hot_unplug)(struct ib_uobject *uobj);
};

struct uverbs_obj_fd_type {
	/*
	 * In fd based objects, uverbs_obj_type_ops points to a generic
	 * fd operations. In order to specialize the underlying types (e.g.
	 * completion_channel), we use obj_size, fops, name and flags for fd
	 * creation and hot_unplug for specific release callback.
	 */
	struct uverbs_obj_type  type;
	size_t			obj_size;
	void (*hot_unplug)(struct ib_uobject_file *uobj_file,
			   bool device_removed);
	const struct file_operations	*fops;
	const char			*name;
	int				flags;
};

extern const struct uverbs_obj_type_ops uverbs_idr_ops;

#define UVERBS_BUILD_BUG_ON(cond) (sizeof(char[1 - 2 * !!(cond)]) -	\
				   sizeof(char))
#define UVERBS_TYPE_ALLOC_IDR_SZ(_size, _order, _hot_unplug)		\
	 {.type = {							\
		.destroy_order = _order,				\
		.ops = &uverbs_idr_ops,					\
	 },								\
	 .hot_unplug = _hot_unplug,					\
	 .obj_size = (_size) +						\
		UVERBS_BUILD_BUG_ON((_size) < sizeof(struct		\
						     ib_uobject))}
#define UVERBS_TYPE_ALLOC_IDR(_order, _hot_unplug)			\
	 UVERBS_TYPE_ALLOC_IDR_SZ(sizeof(struct ib_uobject), _order,	\
				  _hot_unplug)
#endif
