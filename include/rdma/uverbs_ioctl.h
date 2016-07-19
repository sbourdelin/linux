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

#define UVERBS_ATTR(_id, _len, _type)					\
	[_id] = {.len = _len, .type = _type}
#define UVERBS_ATTR_PTR_IN(_id, _len)					\
	UVERBS_ATTR(_id, _len, UVERBS_ATTR_TYPE_PTR_IN)
#define UVERBS_ATTR_PTR_OUT(_id, _len)					\
	UVERBS_ATTR(_id, _len, UVERBS_ATTR_TYPE_PTR_OUT)
#define UVERBS_ATTR_IDR_SZ_IN(_id, _idr_type, _access, _new_sz)		\
	[_id] = {.type = UVERBS_ATTR_TYPE_IDR,				\
		 .idr = {.idr_type = _idr_type,				\
			 .access = _access,				\
			 .new_size = _new_sz} }
#define UVERBS_ATTR_IDR_IN(_id, _idr_type, _access)			\
	UVERBS_ATTR_IDR_SZ_IN(_id, _idr_type, _access, sizeof(struct ib_uobject))
#define UVERBS_ATTR_CHAIN_SPEC_SZ(...)					\
	(sizeof((const struct uverbs_attr_spec[]){__VA_ARGS__}) /	\
	 sizeof(const struct uverbs_attr_spec))
#define UVERBS_ATTR_CHAIN_SPEC(...)					\
	(const struct uverbs_attr_chain_spec)				\
	{.attrs = (struct uverbs_attr_spec[]){__VA_ARGS__},		\
	 .num_attrs = UVERBS_ATTR_CHAIN_SPEC_SZ(__VA_ARGS__)}
#define DECLARE_UVERBS_ATTR_CHAIN_SPEC(name, ...)			\
	const struct uverbs_attr_chain_spec name =			\
		UVERBS_ATTR_CHAIN_SPEC(__VA_ARGS__)
#define UVERBS_ATTR_ACTION_SPEC_SZ(...)					  \
	(sizeof((const struct uverbs_attr_chain_spec *[]){__VA_ARGS__}) / \
				 sizeof(const struct uverbs_attr_chain_spec *))
#define UVERBS_ATTR_ACTION_SPEC(_distfn, _priv, ...)			\
	{.dist = _distfn,						\
	 .priv = _priv,							\
	 .num_chains =	UVERBS_ATTR_ACTION_SPEC_SZ(__VA_ARGS__),	\
	 .validator_chains = (const struct uverbs_attr_chain_spec *[]){__VA_ARGS__} }
#define UVERBS_STD_ACTION_SPEC(...)						\
	UVERBS_ATTR_ACTION_SPEC(ib_uverbs_std_dist,				\
				(void *)UVERBS_ATTR_ACTION_SPEC_SZ(__VA_ARGS__),\
				__VA_ARGS__)
#define UVERBS_STD_ACTION(_handler, _priv, ...)				\
	{								\
		.priv = &(struct uverbs_action_std_handler)		\
			{.handler = _handler,				\
			 .priv = _priv},				\
		.handler = uverbs_action_std_handle,			\
		.chain = UVERBS_STD_ACTION_SPEC(__VA_ARGS__)}
#define UVERBS_STD_CTX_ACTION(_handler, _priv, ...)			\
	{								\
		.priv = &(struct uverbs_action_std_ctx_handler)		\
			{.handler = _handler,				\
			 .priv = _priv},				\
		.handler = uverbs_action_std_ctx_handle,		\
		.chain = UVERBS_STD_ACTION_SPEC(__VA_ARGS__)}
#define UVERBS_ACTIONS_SZ(...)					\
	(sizeof((const struct uverbs_action []){__VA_ARGS__}) /		\
	 sizeof(const struct uverbs_action))
#define UVERBS_ACTION(action_idx, _handler, _priv,  ...)		\
	[action_idx] = UVERBS_STD_ACTION(_handler, _priv, __VA_ARGS__)
#define UVERBS_CTX_ACTION(action_idx, _handler, _priv,  ...)		\
	[action_idx] = UVERBS_STD_CTX_ACTION(_handler, _priv, __VA_ARGS__)
#define UVERBS_ACTIONS(...)					\
	((const struct uverbs_type_actions)				\
	  {.num_actions = UVERBS_ACTIONS_SZ(__VA_ARGS__),		\
	   .actions = (const struct uverbs_action[]){__VA_ARGS__} })
#define DECLARE_UVERBS_TYPE(name, ...)					\
	const struct uverbs_type_actions name = UVERBS_ACTIONS(__VA_ARGS__)
#define UVERBS_TYPES_SZ(...)						\
	(sizeof((const struct uverbs_type_actions *[]){__VA_ARGS__}) /	\
	 sizeof(const struct uverbs_type_actions *))
#define UVERBS_TYPE_ACTIONS(type_idx, ...)				\
	[type_idx] = &UVERBS_ACTIONS(__VA_ARGS__)
#define UVERBS_TYPE(type_idx, type_ptr)					\
	[type_idx] = ((const struct uverbs_type_actions * const)&type_ptr)
#define UVERBS_TYPES(...)						\
	{.num_types = UVERBS_TYPES_SZ(__VA_ARGS__),			\
	 .types = (const struct uverbs_type_actions *[]){__VA_ARGS__} }

#define UVERBS_COPY_TO(attr_array, idx, from)				\
	((attr_array)->attrs[idx].valid ?				\
	 (copy_to_user((attr_array)->attrs[idx].cmd_attr.ptr, (from),	\
		       (attr_array)->attrs[idx].cmd_attr.len) ?		\
	  -EFAULT : 0) : -ENOENT)

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
