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

enum uverbs_common_types {
	UVERBS_TYPE_PD,
	UVERBS_TYPE_COMP_CHANNEL,
	UVERBS_TYPE_CQ,
	UVERBS_TYPE_QP,
	UVERBS_TYPE_SRQ,
	UVERBS_TYPE_AH,
	UVERBS_TYPE_MR,
	UVERBS_TYPE_MW,
	UVERBS_TYPE_FLOW,
	UVERBS_TYPE_XRCD,
	UVERBS_TYPE_RWQ_IND_TBL,
	UVERBS_TYPE_WQ,
	UVERBS_TYPE_LAST,
};

extern const struct uverbs_type uverbs_type_cq;
extern const struct uverbs_type uverbs_type_qp;
extern const struct uverbs_type uverbs_type_rwq_ind_table;
extern const struct uverbs_type uverbs_type_wq;
extern const struct uverbs_type uverbs_type_srq;
extern const struct uverbs_type uverbs_type_ah;
extern const struct uverbs_type uverbs_type_flow;
extern const struct uverbs_type uverbs_type_comp_channel;
extern const struct uverbs_type uverbs_type_mr;
extern const struct uverbs_type uverbs_type_mw;
extern const struct uverbs_type uverbs_type_pd;
extern const struct uverbs_type uverbs_type_xrcd;
extern const struct uverbs_type_group uverbs_common_types;
#endif

