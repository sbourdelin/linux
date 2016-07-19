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

#ifndef _UVERBS_IOCTL_CMD_
#define _UVERBS_IOCTL_CMD_

#include <rdma/uverbs_ioctl.h>

int ib_uverbs_std_dist(__u16 *attr_id, void *priv);

/* common validators */

int uverbs_action_std_handle(struct ib_device *ib_dev,
			     struct ib_uverbs_file *ufile,
			     struct uverbs_attr_array *ctx, size_t num,
			     void *_priv);
int uverbs_action_std_ctx_handle(struct ib_device *ib_dev,
				 struct ib_uverbs_file *ufile,
				 struct uverbs_attr_array *ctx, size_t num,
				 void *_priv);

struct uverbs_action_std_handler {
	int (*handler)(struct ib_device *ib_dev, struct ib_ucontext *ucontext,
		       struct uverbs_attr_array *common,
		       struct uverbs_attr_array *vendor,
		       void *priv);
	void *priv;
};

struct uverbs_action_std_ctx_handler {
	int (*handler)(struct ib_device *ib_dev, struct ib_uverbs_file *ufile,
		       struct uverbs_attr_array *common,
		       struct uverbs_attr_array *vendor,
		       void *priv);
	void *priv;
};

int rdma_initialize_common_types(struct ib_device *ib_dev, unsigned int types);

enum uverbs_common_types {
	UVERBS_TYPE_DEVICE, /* Don't use IDRs here */
	UVERBS_TYPE_PD,
	UVERBS_TYPE_CQ,
	UVERBS_TYPE_QP,
	UVERBS_TYPE_SRQ,
	UVERBS_TYPE_AH,
	UVERBS_TYPE_MR,
	UVERBS_TYPE_MW,
	UVERBS_TYPE_FLOW,
	UVERBS_TYPE_XRCD,
};

#define UVERBS_COMMON_TYPES						       \
	(UVERBS_TYPE_PD | UVERBS_TYPE_CQ | UVERBS_TYPE_QP | UVERBS_TYPE_SRQ |  \
	 UVERBS_TYPE_AH | UVERBS_TYPE_MR | UVERBS_TYPE_MW | UVERBS_TYPE_FLOW | \
	 UVERBS_TYPE_XRCD)

enum uverbs_create_qp_cmd_attr {
	CREATE_QP_CMD,
	CREATE_QP_RESP,
	CREATE_QP_QP,
	CREATE_QP_PD,
	CREATE_QP_RECV_CQ,
	CREATE_QP_SEND_CQ,
};

enum uverbs_destroy_qp_cmd_attr {
	DESTROY_QP_RESP,
	DESTROY_QP_QP,
};

enum uverbs_create_cq_cmd_attr {
	CREATE_CQ_CMD,
	CREATE_CQ_RESP,
};

enum uverbs_get_context {
	GET_CONTEXT_RESP,
};

enum uverbs_query_device {
	QUERY_DEVICE_RESP,
	QUERY_DEVICE_ODP,
	QUERY_DEVICE_TIMESTAMP_MASK,
	QUERY_DEVICE_HCA_CORE_CLOCK,
	QUERY_DEVICE_CAP_FLAGS,
};

extern const struct uverbs_attr_chain_spec uverbs_get_context_spec;
extern const struct uverbs_attr_chain_spec uverbs_query_device_spec;

int uverbs_get_context(struct ib_device *ib_dev,
		       struct ib_uverbs_file *file,
		       struct uverbs_attr_array *common,
		       struct uverbs_attr_array *vendor,
		       void *priv);

int uverbs_query_device_handler(struct ib_device *ib_dev,
				struct ib_ucontext *ucontext,
				struct uverbs_attr_array *common,
				struct uverbs_attr_array *vendor,
				void *priv);

#endif

