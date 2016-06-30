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

#ifndef UOBJECT_H
#define UOBJECT_H

struct uverbs_lock_class {
	struct lock_class_key	key;
	char			name[16];
};

struct uverbs_uobject_type {
	struct list_head	type_list;
	void (*free)(struct uverbs_uobject_type *uobject_type,
		     struct ib_uobject *uobject,
		     struct ib_ucontext *ucontext);
	u16			obj_type;
	struct uverbs_lock_class lock_class;
};

/* embed in ucontext per type */
struct uverbs_uobject_list {
	struct uverbs_uobject_type	*type;
	struct list_head		list;
	struct list_head		type_list;
};

int ib_uverbs_uobject_add(struct ib_uobject *uobject,
			  struct uverbs_uobject_type *uobject_type);
void ib_uverbs_uobject_remove(struct ib_uobject *uobject);
void ib_uverbs_uobject_enable(struct ib_uobject *uobject);

void init_uobj(struct ib_uobject *uobj, u64 user_handle,
	       struct ib_ucontext *context, struct uverbs_lock_class *c);

void release_uobj(struct kref *kref);
void put_uobj(struct ib_uobject *uobj);
void put_uobj_read(struct ib_uobject *uobj);
void put_uobj_write(struct ib_uobject *uobj);

static inline void put_pd_read(struct ib_pd *pd)
{
	put_uobj_read(pd->uobject);
}

static inline void put_cq_read(struct ib_cq *cq)
{
	put_uobj_read(cq->uobject);
}

static inline void put_ah_read(struct ib_ah *ah)
{
	put_uobj_read(ah->uobject);
}

static inline void put_qp_read(struct ib_qp *qp)
{
	put_uobj_read(qp->uobject);
}

static inline void put_qp_write(struct ib_qp *qp)
{
	put_uobj_write(qp->uobject);
}

static inline void put_srq_read(struct ib_srq *srq)
{
	put_uobj_read(srq->uobject);
}

static inline void put_xrcd_read(struct ib_uobject *uobj)
{
	put_uobj_read(uobj);
}
#endif /* UIDR_H */
