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

struct ib_uobject;

enum uverbs_attr_type {
	UVERBS_ATTR_TYPE_IDR,
	UVERBS_ATTR_TYPE_FD,
};

enum uverbs_idr_access {
	UVERBS_ACCESS_READ,
	UVERBS_ACCESS_WRITE,
	UVERBS_ACCESS_NEW,
	UVERBS_ACCESS_DESTROY
};

struct uverbs_type_alloc_action;
typedef void (*free_type)(const struct uverbs_type_alloc_action *uobject_type,
			  struct ib_uobject *uobject);

struct uverbs_type_alloc_action {
	enum uverbs_attr_type		type;
	int				order;
	size_t				obj_size;
	free_type			free_fn;
	struct {
		const struct file_operations	*fops;
		const char			*name;
		int				flags;
	} fd;
};

#endif
