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

#include "uobject.h"
#include "uidr.h"

/*
 * The ib_uobject locking scheme is as follows:
 *
 * - uobj->context->device->idr_lock protects the uverbs idrs themselves, so
 *   it needs to be held during all idr write operations.  When an object is
 *   looked up, a reference must be taken on the object's kref before
 *   dropping this lock.  For read operations, the rcu_read_lock()
 *   and rcu_write_lock() but similarly the kref reference is grabbed
 *   before the rcu_read_unlock().
 *
 * - Each object also has an rwsem.  This rwsem must be held for
 *   reading while an operation that uses the object is performed.
 *   For example, while registering an MR, the associated PD's
 *   uobject.mutex must be held for reading.  The rwsem must be held
 *   for writing while initializing or destroying an object.
 *
 * - In addition, each object has a "live" flag.  If this flag is not
 *   set, then lookups of the object will fail even if it is found in
 *   the idr.  This handles a reader that blocks and does not acquire
 *   the rwsem until after the object is destroyed.  The destroy
 *   operation will set the live flag to 0 and then drop the rwsem;
 *   this will allow the reader to acquire the rwsem, see that the
 *   live flag is 0, and then drop the rwsem and its reference to
 *   object.  The underlying storage will not be freed until the last
 *   reference to the object is dropped.
 */

void init_uobj(struct ib_uobject *uobj, u64 user_handle,
	       struct ib_ucontext *context, struct uverbs_lock_class *c)
{
	uobj->user_handle = user_handle;
	uobj->context     = context;
	kref_init(&uobj->ref);
	init_rwsem(&uobj->mutex);
	lockdep_set_class_and_name(&uobj->mutex, &c->key, c->name);
	uobj->live        = 0;
}

void release_uobj(struct kref *kref)
{
	kfree_rcu(container_of(kref, struct ib_uobject, ref), rcu);
}

void put_uobj(struct ib_uobject *uobj)
{
	kref_put(&uobj->ref, release_uobj);
}

void put_uobj_read(struct ib_uobject *uobj)
{
	up_read(&uobj->mutex);
	put_uobj(uobj);
}

void put_uobj_write(struct ib_uobject *uobj)
{
	up_write(&uobj->mutex);
	put_uobj(uobj);
}
