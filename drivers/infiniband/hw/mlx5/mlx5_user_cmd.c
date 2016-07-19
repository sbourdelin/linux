/*
 * Copyright (c) 2016, Mellanox Technologies. All rights reserved.
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

#include <rdma/uverbs_ioctl_cmd.h>
#include "user.h"

/* Refactor this to separate the versions */
enum mlx5_alloc_ucontext_args {
	ALLOC_UCONTEXT_IN,
	ALLOC_UCONTEXT_OUT,
};

enum mlx5_device_actions {
	DEVICE_ALLOC_CONTEXT,
	DEVICE_QUERY,
};

DECLARE_UVERBS_TYPE(
	mlx5_device,
	UVERBS_CTX_ACTION(
		DEVICE_ALLOC_CONTEXT, uverbs_get_context, NULL,
		&uverbs_get_context_spec,
		&UVERBS_ATTR_CHAIN_SPEC(
			/*
			 * Declared with size 0 as we current provide
			 * backward compatibility (0 = variable size)
			 */
			UVERBS_ATTR_PTR_IN(ALLOC_UCONTEXT_IN, 0),
			UVERBS_ATTR_PTR_OUT(ALLOC_UCONTEXT_OUT, 0),
			),
	),
	UVERBS_ACTION(
		DEVICE_QUERY, uverbs_query_device_handler, NULL,
		&uverbs_query_device_spec,
	),
);

struct uverbs_types mlx5_types = UVERBS_TYPES(
	UVERBS_TYPE(UVERBS_TYPE_DEVICE, mlx5_device)
);
