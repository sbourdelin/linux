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

#include <rdma/ib_verbs.h>
#include <linux/mutex.h>

struct uverbs_uobject_type *uverbs_get_type(struct ib_device *ibdev,
					    uint16_t type);
int ib_uverbs_uobject_type_add(struct list_head	*head,
			       void (*free)(struct uverbs_uobject_type *uobject_type,
					    struct ib_uobject *uobject,
					    struct ib_ucontext *ucontext),
			       uint16_t	obj_type);

struct uverbs_uobject_type {
	struct list_head	type_list;
	void (*free)(struct uverbs_uobject_type *uobject_type,
		     struct ib_uobject *uobject,
		     struct ib_ucontext *ucontext);
	u16			obj_type;
	size_t			obj_size;
};

/* embed in ucontext per type */
struct uverbs_uobject_list {
	struct uverbs_uobject_type	*type;
	/* lock of the uobject data type */
	struct mutex			uobj_lock;
	struct list_head		list;
	struct list_head		type_list;
};

#endif /* UIDR_H */
