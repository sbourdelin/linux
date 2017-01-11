/*
 * Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2005, 2006 Cisco Systems.  All rights reserved.
 * Copyright (c) 2005-2016 Mellanox Technologies. All rights reserved.
 * Copyright (c) 2005 Voltaire, Inc. All rights reserved.
 * Copyright (c) 2005 PathScale, Inc. All rights reserved.
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

#ifndef RDMA_CORE_H
#define RDMA_CORE_H

#include <linux/idr.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/ib_verbs.h>
#include <linux/mutex.h>

/*
 * Get an ib_uobject that corresponds to the given id from ucontext, assuming
 * the object is from the given type. Lock it to the required access.
 * This function could create (access == NEW) or destroy (access == DESTROY)
 * objects if required. The action will be finalized only when
 * uverbs_finalize_object is called.
 */
struct ib_uobject *uverbs_get_uobject_from_context(const struct uverbs_type_alloc_action *type_alloc,
						   struct ib_ucontext *ucontext,
						   enum uverbs_idr_access access,
						   unsigned int id);

/* Check if the object is still alive. This must be called within RCU */
bool uverbs_is_live(struct ib_uobject *uobj);
void uverbs_finalize_object(struct ib_uobject *uobj,
			    enum uverbs_idr_access access,
			    bool success);
/*
 * These functions initialize and destroy the context. The context has a
 * list of objects which is protected by a kref-ed lock, whose purpose is
 * to protect concurrent FDs (e.g completion channel FDs) release while
 * traversing the context and releasing its objects. initialize_ucontext
 * should be called when we create a context. cleanup_ucontext removes all
 * objects created in the ucontext. release_ucontext drops the reference from
 * the lock.
 */
void uverbs_cleanup_ucontext(struct ib_ucontext *ucontext,
			     const struct uverbs_root *root);
int uverbs_initialize_ucontext(struct ib_ucontext *ucontext);
void uverbs_release_ucontext(struct ib_ucontext *ucontext);

/*
 * Indicate this fd is no longer used by this consumer, but its memory isn't
 * released yet. The memory is released only when ib_uverbs_cleanup_fd is
 * called.
 */
void uverbs_close_fd(struct file *f);
void uverbs_cleanup_fd(void *private_data);

static inline void *uverbs_fd_uobj_to_priv(struct ib_uobject *uobj)
{
	return uobj + 1;
}

#endif /* RDMA_CORE_H */
