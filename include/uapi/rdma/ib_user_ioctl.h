/*
 * Copyright (c) 2016 Mellanox Technologies, LTD. All rights reserved.
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

#ifndef IB_USER_IOCTL_H
#define IB_USER_IOCTL_H

#include <linux/types.h>
#include <linux/ioctl.h>

struct ib_uverbs_uptr {
	__u64 ptr;
	__u32 len;
};

struct ib_uverbs_ioctl_hdr {
	__u32 length;
	__u16 flags;
	__u16 object_type;
	__u16 reserved;
	__u16 action;
	__u32 user_handler;
	/*
	 * These fields represent core response only,
	 * provider's response is given as a netlink attribute.
	 */
	struct ib_uverbs_uptr resp;
};

enum ib_uverbs_object_type {
	IB_OBJ_TYPE_OBJECT, /* query supported types */
	IB_OBJ_TYPE_DEVICE,
	IB_OBJ_TYPE_QP,
	IB_OBJ_TYPE_CQ,
	IB_OBJ_TYPE_PD,
	IB_OBJ_TYPE_MR,
	IB_OBJ_TYPE_MW,
	IB_OBJ_TYPE_FLOW,
	IB_OBJ_TYPE_MAX
};

enum ib_uverbs_object_type_flags {
	/* vendor flag should go here */
	IB_UVERBS_OBJECT_TYPE_FLAGS_MAX = 1 << 0,
};

enum ib_uverbs_common_actions {
	IBNL_OBJECT_CREATE,
	IBNL_OBJECT_DESTROY,
	IBNL_OBJECT_QUERY,
	IBNL_OBJECT_MODIFY,
	IBNL_OBJECT_MAX = 8
};

/* Couldn't be extended! */
enum ibnl_vendor_attrs {
	IBNL_PROVIDER_CMD_UPTR,
	IBNL_PROVIDER_RESP_UPTR,
	IBNL_VENDOR_ATTRS_MAX
};

enum ib_uverbs_common_resp_types {
	IBNL_RESPONSE_TYPE_RESP,
	IBNL_RESPONSE_TYPE_VENDOR,
	IBNL_RESPONSE_TYPE_MAX = 8
};

#define IB_IOCTL_MAGIC		0x1b

#define IB_CMD_VERBS		0x1
#define IB_CMD_DIRECT		0x2

#define IB_IOCTL_VERBS \
	_IOWR(IB_IOCTL_MAGIC, IB_CMD_VERBS, struct ib_uverbs_ioctl_hdr)

#define IB_IOCTL_DIRECT \
	_IOWR(IB_IOCTL_MAGIC, IB_CMD_DIRECT, unsigned long)

/* Legacy part
 * !!!! NOTE: It uses the same command index as VERBS
 */
#include <rdma/ib_user_mad.h>
#define IB_USER_MAD_REGISTER_AGENT	_IOWR(IB_IOCTL_MAGIC, 1, \
					      struct ib_user_mad_reg_req)

#define IB_USER_MAD_UNREGISTER_AGENT	_IOW(IB_IOCTL_MAGIC, 2, __u32)

#define IB_USER_MAD_ENABLE_PKEY		_IO(IB_IOCTL_MAGIC, 3)

#define IB_USER_MAD_REGISTER_AGENT2     _IOWR(IB_IOCTL_MAGIC, 4, \
					      struct ib_user_mad_reg_req2)

enum ibnl_create_device {
	IBNL_CREATE_DEVICE_CORE = IBNL_VENDOR_ATTRS_MAX,
	IBNL_CREATE_DEVICE_MAX
};

#endif /* IB_USER_IOCTL_H */
