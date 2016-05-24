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

#ifndef RDMA_IOCTL_H
#define RDMA_IOCTL_H

#include <linux/types.h>
#include <rdma/ib_user_verbs.h>
#include <linux/ioctl.h>


/* ioctls are grouped into 1 of 8 domains/namespaces
#define URDMA_DOMAIN(nr)	(_IOC_NR(nr) >> 5)
enum {
	URDMA_DOMAIN_OBJECT,
	URDMA_DOMAIN_DRIVER,
	URDMA_MAX_DOMAIN
};
*/

#define URDMA_OP_MASK			0x7F
#define URDMA_OP(nr)			(_IOC_NR(nr) & URDMA_OP_MASK)

/* operations */
enum {
	URDMA_QUERY,
	URDMA_OPEN,
	URDMA_CLOSE,
	URDMA_MODIFY,
	URDMA_READ,
	URDMA_WRITE,
	URDMA_MAX_OP
};

/* driver specific object operations set the high-order op bit */
#define URDMA_DRIVER_OP			(0x80)

/* operation domains, doubles as object types */
enum {
	URDMA_DRIVER,	/* is this usable? */
	URDMA_DEVICE,
	URDMA_PORT,
	URDMA_CQ,
	URDMA_PD,
	URDMA_AH,
	URDMA_MR,
	URDMA_SHARED_RX,
	URDMA_SHARED_TX,
	URDMA_QP,
	URDMA_CMD_CTX,
	/* others... */
	URDMA_MAX_DOMAIN
};

/* driver specific domains set the high-order domain bit */
#define URDMA_DRIVER_DOMAIN		(1 << 16)

struct urdma_obj_id {
	u32	instance_id;
	u16	obj_type;
	u16	resv;
};

/* ensure that data beyond header starts at 64-byte alignment */
struct urdma_ioctl {
	u8	version;
	u8	count;
	u16	domain;
	u16	length;
	u16	resv;
	u64	flags;
	union {
		struct urdma_obj_id	obj_id[0];
		u64			data[0];
#ifdef __KERNEL__
		void			*obj[0];
#endif
	};
};


#define URDMA_TYPE			0xda
#define URDMA_IO(op)			_IO(URDMA_TYPE, op)
#define URDMA_IOR(op, type)		_IOR(URDMA_TYPE, op, type)
#define URDMA_IOW(op, type)		_IOW(URDMA_TYPE, op, type)
#define URDMA_IOWR(op, type)		_IOWR(URDMA_TYPE, op, type)

#define URDMA_DRIVER_CMD(op)		(op | URDMA_DRIVER_OP)
#define URDMA_DRIVER_IO(op)		URDMA_IO(URDMA_DRIVER_CMD(op))
#define URDMA_DRIVER_IOR(op, type)	URDMA_IOR(URDMA_DRIVER_CMD(op), type)
#define URDMA_DRIVER_IOW(op, type)	URDMA_IOW(URDMA_DRIVER_CMD(op), type)
#define URDMA_DRIVER_IOWR(op, type)	URDMA_IOWR(URDMA_DRIVER_CMD(op), type)

#define URDMA_IOCTL(op)			URDMA_IOWR(URDMA_##op, struct urdma_ioctl)

#define URDMA_IOCTL_QUERY		URDMA_IOCTL(QUERY)
#define URDMA_IOCTL_OPEN		URDMA_IOCTL(OPEN)
#define URDMA_IOCTL_CLOSE		URDMA_IOCTL(CLOSE)
#define URDMA_IOCTL_MODIFY		URDMA_IOCTL(MODIFY)
#define URDMA_IOCTL_READ		URDMA_IOCTL(READ)
#define URDMA_IOCTL_WRITE		URDMA_IOCTL(WRITE)


#endif /* RDMA_IOCTL_H */

