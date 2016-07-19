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

#ifndef _UVERBS_IOCTL_
#define _UVERBS_IOCTL_

#include <linux/kernel.h>

struct uverbs_object_type;
struct ib_ucontext;
struct ib_device;

/*
 * =======================================
 *	Verbs action specifications
 * =======================================
 */

enum uverbs_attr_type {
	UVERBS_ATTR_TYPE_PTR_IN,
	UVERBS_ATTR_TYPE_PTR_OUT,
	UVERBS_ATTR_TYPE_IDR,
	/*
	 * TODO: we could add FD type for command which will migrate the events
	 * to a specific FD.
	 */
};

enum uverbs_idr_access {
	UVERBS_IDR_ACCESS_READ,
	UVERBS_IDR_ACCESS_WRITE,
	UVERBS_IDR_ACCESS_NEW,
	UVERBS_IDR_ACCESS_DESTROY
};

struct uverbs_attr_spec {
	u16				len;
	enum uverbs_attr_type		type;
	struct {
		u16			new_size;
		u16			idr_type;
		u8			access;
	} idr;
	/* TODO: In case of FD, we could validate here the fops pointer */
};

struct uverbs_attr_chain_spec {
	struct uverbs_attr_spec		*attrs;
	size_t				num_attrs;
};

struct action_spec {
	const struct uverbs_attr_chain_spec		**validator_chains;
	/* if > 0 -> validator, otherwise, error */
	int (*dist)(__u16 *attr_id, void *priv);
	void						*priv;
	size_t						num_chains;
};

struct uverbs_attr_array;
struct ib_uverbs_file;

struct uverbs_action {
	struct action_spec chain;
	void *priv;
	int (*handler)(struct ib_device *ib_dev, struct ib_uverbs_file *ufile,
		       struct uverbs_attr_array *ctx, size_t num, void *priv);
};

struct uverbs_type_actions {
	size_t				num_actions;
	const struct uverbs_action	*actions;
};

struct uverbs_types {
	size_t					num_types;
	const struct uverbs_type_actions	**types;
};

/* =================================================
 *              Parsing infrastructure
 * =================================================
 */

struct uverbs_ptr_attr {
	void	* __user ptr;
	__u16		len;
};

struct uverbs_obj_attr {
	/*  idr handle */
	__u32	idr;
	/* pointer to the kernel descriptor -> type, access, etc */
	const struct uverbs_attr_spec *val;
	struct ib_uobject		*uobject;
	struct uverbs_uobject_type	*type;
};

struct uverbs_attr {
	bool valid;
	union {
		struct uverbs_ptr_attr	cmd_attr;
		struct uverbs_obj_attr	obj_attr;
	};
};

/* output of one validator */
struct uverbs_attr_array {
	size_t num_attrs;
	/* arrays of attrubytes, index is the id i.e SEND_CQ */
	struct uverbs_attr *attrs;
};

/* =================================================
 *              Types infrastructure
 * =================================================
 */

int ib_uverbs_uobject_type_add(struct list_head	*head,
			       void (*free)(struct uverbs_uobject_type *uobject_type,
					    struct ib_uobject *uobject,
					    struct ib_ucontext *ucontext),
			       uint16_t	obj_type);
void ib_uverbs_uobject_types_remove(struct ib_device *ib_dev);

#endif
