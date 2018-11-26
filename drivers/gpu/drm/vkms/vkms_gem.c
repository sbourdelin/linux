// SPDX-License-Identifier: GPL-2.0
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/shmem_fs.h>
#include <drm/drm_gem_shmem_helper.h>

#include "vkms_drv.h"

struct drm_gem_object *vkms_gem_create(struct drm_device *dev,
				       struct drm_file *file,
				       u32 *handle,
				       u64 size)
{
	struct drm_gem_shmem_object *obj;
	int ret;

	if (!file || !dev || !handle)
		return ERR_PTR(-EINVAL);

	obj = drm_gem_shmem_create(dev, size);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	/*
	 * Allocate an id of idr table where the obj is registered
	 * and handle has the id what user can see.
	 */
	ret = drm_gem_handle_create(file, &obj->base, handle);
	/* drop reference from allocate - handle holds it now. */
	drm_gem_object_put_unlocked(&obj->base);
	if (ret)
		return ERR_PTR(ret);

	return &obj->base;
}
