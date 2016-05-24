/*
 * Copyright (c) 2016 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
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

#ifndef RDMA_UAPI_H
#define RDMA_UAPI_H

#include <linux/types.h>
#include <rdma/ib_user_verbs.h>
#include <uapi/rdma/rdma_ioctl.h>


#define URDMA_OFFSET(dom, op)		(dom * URDMA_MAX_OP + op)
#define URDMA_MAX_BASE			(URDMA_MAX_DOMAIN * URDMA_MAX_OP - 1)
#define URDMA_DRIVER_OFFSET(op)		(op)

/* Object and control flags */
/* Operation on object requires exclusive access - e.g. MODIFY */
#define URDMA_EXCL		(1 << 0)
/* Events may be generated for the given object - e.g. CQ, QP */
#define URDMA_EVENT		(1 << 1)
/* Device resources have been freed */
#define URDMA_CLOSED		(1 << 2)

struct urdma_device;

typedef long (*urdma_ioctl_handler_t)(struct urdma_device *dev,
				      void *data, void *file_data);

/* The purpose of this structure is to guide the behavior of the
 * common ioctl processing code.
 */
struct urdma_ioctl_desc {
	u64			flags;
	unsigned int		cmd;
	u16			length; /* max size needed */
	urdma_ioctl_handler_t	func;
	const char		*name;
};

typedef long (*urdma_ioctl_hook_t)(struct urdma_device *dev,
				   struct urdma_ioctl *ioctl,
				   struct urdma_ioctl_desc *desc,
				   void *file_data);

#define URDMA_DESC(_dom, _op, _func, _flags)			\
	[URDMA_OFFSET(URDMA_##_dom, URDMA_##_op)] = {		\
		.flags = _flags,				\
		.cmd = URDMA_IOCTL_##_op,			\
		.func = _func,					\
		.name = #_dom "_" #_op				\
	}

#define URDMA_DRIVER_DESC(_dom, _op, _func, _flags)		\
	[URDMA_DRIVER_OFFSET(URDMA_##_dom, URDMA_##_op)] = {	\
		.flags = _flags,				\
		.cmd = URDMA_IOCTL_##_op,			\
		.func = _func,					\
		.name = #_dom "_" #_op				\
	}

extern const struct urdma_ioctl_desc verbs_ioctl[URDMA_MAX_BASE];

struct urdma_driver {
	int			num_ioctls;
	struct urdma_ioctl_desc	*ioctl;
};

/* will merge with ib_device, can be separated later to support
 * non-verbs devices that do not plug into the kernel APIs
 */
struct urdma_device {
	struct urdma_driver	*drv;
	struct rw_semaphore	rw_lock;
	int			flags;
	int			num_ioctls;
	struct urdma_ioctl_desc	*ioctl;

	/* Order to cleanup obj_list.  Objects are destroyed from
	 * obj_list[close_map[0]]..obj_list[close_map[n]]
	 */
	int			num_objs;
	int			*close_map;
	struct list_head	*obj_lists;
};

/* use ib_uobject? */
/* urdma will protect against destroying an object that is in use,
 * but all locking is pushed down to the drivers.
 * Keep this structure as small as possible
 */
struct urdma_obj {
	u64			ucontext;
	void			*kcontext;
	u32			instance_id;	/* idr index */
	u16			obj_type;
	u16			flags;
	struct urdma_device	*dev;
	struct list_head	entry;
	atomic_t		use_cnt;
	//struct kref		ref;
	//struct rw_semaphore	mutex;
};

struct urdma_map {
	struct idr		idr;
	struct mutex		lock;
};


#endif /* RDMA_UAPI_H */
diff --git a/drivers/infiniband/core/urdma.c b/drivers/infiniband/core/urdma.c
