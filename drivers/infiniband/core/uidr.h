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

#ifndef UIDR_H
#define UIDR_H

#include <linux/idr.h>
#include <rdma/uverbs_ioctl.h>

struct uverbs_uobject_type *uverbs_get_type(struct ib_device *ibdev,
					    uint16_t type);
struct ib_uobject *uverbs_get_type_from_idr(struct uverbs_uobject_type *type,
					    struct ib_ucontext *ucontext,
					    int access,
					    uint32_t idr);
void ib_uverbs_uobject_remove(struct ib_uobject *uobject);
void ib_uverbs_uobject_enable(struct ib_uobject *uobject);
void uverbs_unlock_objects(struct uverbs_attr_array *attr_array,
			   size_t num,
			   const struct action_spec *chain,
			   bool success);
int idr_add_uobj(struct ib_uobject *uobj);
void idr_remove_uobj(struct ib_uobject *uobj);
struct ib_uobject *idr_write_uobj(int id, struct ib_ucontext *context);

struct ib_pd *idr_read_pd(int pd_handle, struct ib_ucontext *context);
struct ib_cq *idr_read_cq(int cq_handle, struct ib_ucontext *context,
			  int nested);
struct ib_ah *idr_read_ah(int ah_handle, struct ib_ucontext *context);
struct ib_qp *idr_read_qp(int qp_handle, struct ib_ucontext *context);
struct ib_qp *idr_write_qp(int qp_handle, struct ib_ucontext *context);
struct ib_srq *idr_read_srq(int srq_handle, struct ib_ucontext *context);
struct ib_xrcd *idr_read_xrcd(int xrcd_handle, struct ib_ucontext *context,
			      struct ib_uobject **uobj);
#endif /* UIDR_H */
