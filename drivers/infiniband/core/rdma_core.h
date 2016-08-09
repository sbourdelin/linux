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

#include <linux/idr.h>
#include <rdma/uverbs_ioctl.h>
#include <rdma/ib_verbs.h>
#include <linux/mutex.h>

const struct uverbs_type *uverbs_get_type(const struct ib_device *ibdev,
					  uint16_t type);
struct ib_uobject *uverbs_get_type_from_idr(const struct uverbs_type_alloc_action *type,
					    struct ib_ucontext *ucontext,
					    enum uverbs_idr_access access,
					    uint32_t idr);
void ib_uverbs_uobject_remove(struct ib_uobject *uobject);
void ib_uverbs_uobject_enable(struct ib_uobject *uobject);
void uverbs_unlock_object(struct ib_uobject *uobj,
			  enum uverbs_idr_access access,
			  bool success);
void uverbs_unlock_objects(struct uverbs_attr_array *attr_array,
			   size_t num,
			   const struct uverbs_action_spec *spec,
			   bool success);

void ib_uverbs_uobject_type_cleanup_ucontext(struct ib_ucontext *ucontext,
					     const struct uverbs_types_group *types_group);
void ib_uverbs_uobject_type_initialize_ucontext(struct ib_ucontext *ucontext);

int ib_uverbs_uobject_add(struct ib_uobject *uobject,
			  const struct uverbs_type_alloc_action *uobject_type);
void ib_uverbs_uobject_remove(struct ib_uobject *uobject);
void ib_uverbs_uobject_enable(struct ib_uobject *uobject);

#endif /* UIDR_H */
