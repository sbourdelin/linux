/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2018 Intel Corporation
 */

#include <linux/uaccess.h>
#include <uapi/drm/i915_drm.h>

#include "i915_user_extensions.h"

int i915_user_extensions(struct i915_user_extension __user *ext,
			 const i915_user_extension_fn *tbl,
			 unsigned long count,
			 void *data)
{
	int err;
	u64 x;

	if (!ext)
		return 0;

	if (get_user(x, &ext->name))
		return -EFAULT;

	err = -EINVAL;
	if (x < count && tbl[x])
		err = tbl[x](ext, data);
	if (err)
		return err;

	if (get_user(x, &ext->next_extension))
		return -EFAULT;

	return i915_user_extensions(u64_to_user_ptr(x), tbl, count, data);
}
