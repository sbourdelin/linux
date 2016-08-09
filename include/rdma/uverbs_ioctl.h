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
struct ib_uobject;
struct ib_device;
struct uverbs_uobject_type;

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

struct uverbs_attr_group_spec {
	struct uverbs_attr_spec		*attrs;
	size_t				num_attrs;
};

struct uverbs_action_spec {
	const struct uverbs_attr_group_spec		**attr_groups;
	/* if > 0 -> validator, otherwise, error */
	int (*dist)(__u16 *attr_id, void *priv);
	void						*priv;
	size_t						num_groups;
};

struct uverbs_attr_array;
struct ib_uverbs_file;

struct uverbs_action {
	struct uverbs_action_spec spec;
	void *priv;
	int (*handler)(struct ib_device *ib_dev, struct ib_uverbs_file *ufile,
		       struct uverbs_attr_array *ctx, size_t num, void *priv);
};

struct uverbs_type_alloc_action;
typedef void (*free_type)(const struct uverbs_type_alloc_action *uobject_type,
			  struct ib_uobject *uobject);

struct uverbs_type_alloc_action {
	free_type			free_fn;
	int				order;
	size_t				obj_size;
};

struct uverbs_type_actions_group {
	size_t					num_actions;
	const struct uverbs_action		**actions;
};

struct uverbs_type {
	size_t					num_groups;
	const struct uverbs_type_actions_group	**action_groups;
	const struct uverbs_type_alloc_action	*alloc;
	int (*dist)(__u16 *action_id, void *priv);
	void					*priv;
};

struct uverbs_types {
	size_t					num_types;
	const struct uverbs_type		**types;
};

struct uverbs_types_group {
	const struct uverbs_types		**type_groups;
	size_t					num_groups;
	int (*dist)(__u16 *type_id, void *priv);
	void					*priv;
};

#define UVERBS_ATTR(_id, _len, _type)					\
	[_id] = {.len = _len, .type = _type}
#define UVERBS_ATTR_PTR_IN(_id, _len)					\
	UVERBS_ATTR(_id, _len, UVERBS_ATTR_TYPE_PTR_IN)
#define UVERBS_ATTR_PTR_OUT(_id, _len)					\
	UVERBS_ATTR(_id, _len, UVERBS_ATTR_TYPE_PTR_OUT)
#define UVERBS_ATTR_IDR_SZ(_id, _idr_type, _access, _new_sz)		\
	[_id] = {.type = UVERBS_ATTR_TYPE_IDR,				\
		 .idr = {.idr_type = _idr_type,				\
			 .access = _access,				\
			 .new_size = _new_sz} }
#define UVERBS_ATTR_IDR(_id, _idr_type, _access)			\
	UVERBS_ATTR_IDR_SZ(_id, _idr_type, _access, sizeof(struct ib_uobject))
#define _UVERBS_ATTR_SPEC_SZ(...)					\
	(sizeof((const struct uverbs_attr_spec[]){__VA_ARGS__}) /	\
	 sizeof(const struct uverbs_attr_spec))
#define UVERBS_ATTR_SPEC(...)					\
	((const struct uverbs_attr_group_spec)				\
	 {.attrs = (struct uverbs_attr_spec[]){__VA_ARGS__},		\
	  .num_attrs = _UVERBS_ATTR_SPEC_SZ(__VA_ARGS__)})
#define DECLARE_UVERBS_ATTR_SPEC(name, ...)			\
	const struct uverbs_attr_group_spec name =			\
		UVERBS_ATTR_SPEC(__VA_ARGS__)
#define _UVERBS_ATTR_ACTION_SPEC_SZ(...)				  \
	(sizeof((const struct uverbs_attr_group_spec *[]){__VA_ARGS__}) / \
	 sizeof(const struct uverbs_attr_group_spec *))
#define _UVERBS_ATTR_ACTION_SPEC(_distfn, _priv, ...)			\
	{.dist = _distfn,						\
	 .priv = _priv,							\
	 .num_groups =	_UVERBS_ATTR_ACTION_SPEC_SZ(__VA_ARGS__),	\
	 .attr_groups = (const struct uverbs_attr_group_spec *[]){__VA_ARGS__} }
#define UVERBS_ACTION_SPEC(...)						\
	_UVERBS_ATTR_ACTION_SPEC(ib_uverbs_std_dist,				\
				(void *)_UVERBS_ATTR_ACTION_SPEC_SZ(__VA_ARGS__),\
				__VA_ARGS__)
#define UVERBS_ACTION(_handler, _priv, ...)				\
	((const struct uverbs_action) {					\
		.priv = &(struct uverbs_action_std_handler)		\
			{.handler = _handler,				\
			 .priv = _priv},				\
		.handler = uverbs_action_std_handle,			\
		.spec = UVERBS_ACTION_SPEC(__VA_ARGS__)})
#define UVERBS_CTX_ACTION(_handler, _priv, ...)			\
	((const struct uverbs_action){					\
		.priv = &(struct uverbs_action_std_ctx_handler)		\
			{.handler = _handler,				\
			 .priv = _priv},				\
		.handler = uverbs_action_std_ctx_handle,		\
		.spec = UVERBS_ACTION_SPEC(__VA_ARGS__)})
#define _UVERBS_ACTIONS_SZ(...)					\
	(sizeof((const struct uverbs_action *[]){__VA_ARGS__}) /	\
	 sizeof(const struct uverbs_action *))
#define ADD_UVERBS_ACTION(action_idx, _handler, _priv,  ...)		\
	[action_idx] = &UVERBS_ACTION(_handler, _priv, __VA_ARGS__)
#define DECLARE_UVERBS_ACTION(name, _handler, _priv, ...)		\
	const struct uverbs_action name =				\
		UVERBS_ACTION(_handler, _priv, __VA_ARGS__)
#define ADD_UVERBS_CTX_ACTION(action_idx, _handler, _priv,  ...)		\
	[action_idx] = &UVERBS_CTX_ACTION(_handler, _priv, __VA_ARGS__)
#define DECLARE_UVERBS_CTX_ACTION(name, _handler, _priv, ...)	\
	const struct uverbs_action name =				\
		UVERBS_CTX_ACTION(_handler, _priv, __VA_ARGS__)
#define ADD_UVERBS_ACTION_PTR(idx, ptr)					\
	[idx] = ptr
#define UVERBS_TYPE_ALLOC_ACTION(_order, _free_fn, _obj_size)		\
	((const struct uverbs_type_alloc_action){.order = _order,	\
						 .free_fn = _free_fn,	\
						 .obj_size = _obj_size})
#define UVERBS_ACTIONS(...)						\
	((const struct uverbs_type_actions_group)			\
	  {.num_actions = _UVERBS_ACTIONS_SZ(__VA_ARGS__),		\
	   .actions = (const struct uverbs_action *[]){__VA_ARGS__} })
#define DECLARE_UVERBS_ACTIONS(name, ...)				\
	const struct  uverbs_type_actions_group name =			\
		UVERBS_ACTIONS(__VA_ARGS__)
#define _UVERBS_ACTIONS_GROUP_SZ(...)					\
	(sizeof((const struct uverbs_type_actions_group*[]){__VA_ARGS__}) / \
	 sizeof(const struct uverbs_type_actions_group *))
#define UVERBS_TYPE_ALLOC_SZ(_size, _order, _free_fn)			\
	((const struct uverbs_type_alloc_action) {			\
		.order = _order,					\
		.obj_size = _size,					\
		.free_fn = _free_fn})
#define UVERBS_TYPE_ALLOC(_order, _free_fn)				\
	 UVERBS_TYPE_ALLOC_SZ(sizeof(struct ib_uobject), _order, _free_fn)
#define _DECLARE_UVERBS_TYPE(name, _alloc, _dist, _priv, ...)		\
	const struct uverbs_type name = {				\
		.alloc = _alloc,					\
		.dist = _dist,						\
		.priv = _priv,						\
		.num_groups = _UVERBS_ACTIONS_GROUP_SZ(__VA_ARGS__),	\
		.action_groups = (const struct uverbs_type_actions_group *[]){__VA_ARGS__} \
	}
#define DECLARE_UVERBS_TYPE(name, _alloc,  ...)			\
	_DECLARE_UVERBS_TYPE(name, _alloc, ib_uverbs_std_dist, NULL, __VA_ARGS__)
#define _UVERBS_TYPE_SZ(...)						\
	(sizeof((const struct uverbs_type *[]){__VA_ARGS__}) /	\
	 sizeof(const struct uverbs_type *))
#define ADD_UVERBS_TYPE_ACTIONS(type_idx, ...)				\
	[type_idx] = &UVERBS_ACTIONS(__VA_ARGS__)
#define ADD_UVERBS_TYPE(type_idx, type_ptr)				\
	[type_idx] = ((const struct uverbs_type * const)&type_ptr)
#define UVERBS_TYPES(...)  ((const struct uverbs_types)			\
	{.num_types = _UVERBS_TYPE_SZ(__VA_ARGS__),			\
	 .types = (const struct uverbs_type *[]){__VA_ARGS__} })
#define DECLARE_UVERBS_TYPES(name, ...)				\
	const struct uverbs_types name = UVERBS_TYPES(__VA_ARGS__)

#define _UVERBS_TYPES_SZ(...)						\
	(sizeof((const struct uverbs_types *[]){__VA_ARGS__}) /	\
	 sizeof(const struct uverbs_types *))

#define UVERBS_TYPES_GROUP(_dist, _priv, ...)				\
	((const struct uverbs_types_group){				\
		.dist = _dist,						\
		.priv = _priv,						\
		.type_groups = (const struct uverbs_types *[]){__VA_ARGS__},	\
		.num_groups = _UVERBS_TYPES_SZ(__VA_ARGS__)})
#define _DECLARE_UVERBS_TYPES_GROUP(name, _dist, _priv, ...)		\
	const struct uverbs_types_group name = UVERBS_TYPES_GROUP(_dist, _priv, __VA_ARGS__)
#define DECLARE_UVERBS_TYPES_GROUP(name, ...)		\
	_DECLARE_UVERBS_TYPES_GROUP(name, ib_uverbs_std_dist, NULL, __VA_ARGS__)

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
	struct ib_uverbs_attr __user	*uattr;
	const struct uverbs_type_alloc_action	*type;
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
