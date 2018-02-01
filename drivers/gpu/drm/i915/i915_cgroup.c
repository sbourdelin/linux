/* SPDX-License-Identifier: MIT */
/*
 * i915_cgroup.c - Linux cgroups integration for i915
 *
 * Copyright (C) 2018 Intel Corporation
 */

#include <linux/cgroup.h>

#include "i915_drv.h"

struct i915_cgroup_data {
	struct cgroup_driver_data base;

	int priority_offset;
};

static inline struct i915_cgroup_data *
cgrp_to_i915(struct cgroup_driver_data *data)
{
	return container_of(data, struct i915_cgroup_data, base);
}

static struct cgroup_driver_data *
i915_cgroup_alloc(struct cgroup_driver *drv)
{
	struct i915_cgroup_data *data;

	data = kzalloc(sizeof *data, GFP_KERNEL);
	return &data->base;
}

static void
i915_cgroup_free(struct cgroup_driver_data *data)
{
	kfree(data);
}

static struct cgroup_driver_funcs i915_cgroup_funcs = {
	.alloc_data = i915_cgroup_alloc,
	.free_data = i915_cgroup_free,
};

int
i915_cgroup_init(struct drm_i915_private *dev_priv)
{
	dev_priv->i915_cgroups = cgroup_driver_init(&i915_cgroup_funcs);
	if (IS_ERR(dev_priv->i915_cgroups))
		return PTR_ERR(dev_priv->i915_cgroups);

	return 0;
}

void
i915_cgroup_shutdown(struct drm_i915_private *dev_priv)
{
	cgroup_driver_release(dev_priv->i915_cgroups);
}

/**
 * i915_cgroup_setparam_ioctl - ioctl to alter i915 settings for a cgroup
 * @dev: DRM device
 * @data: data pointer for the ioctl
 * @file_priv: DRM file handle for the ioctl call
 *
 * Allows i915-specific parameters to be set for a Linux cgroup.
 */
int
i915_cgroup_setparam_ioctl(struct drm_device *dev, void *data,
                         struct drm_file *file)
{
	struct drm_i915_private *dev_priv = to_i915(dev);
	struct drm_i915_cgroup_param *req = data;
	struct cgroup *cgrp;
	struct file *f;
	struct inode *inode = NULL;
	struct cgroup_driver_data *cgrpdata;
	struct i915_cgroup_data *i915data;
	int ret;

	if (!dev_priv->i915_cgroups) {
		DRM_DEBUG_DRIVER("No support for driver-specific cgroup data\n");
		return -EINVAL;
	}

	/* We don't actually support any flags yet. */
	if (req->flags) {
		DRM_DEBUG_DRIVER("Invalid flags\n");
		return -EINVAL;
	}

	/*
	 * Make sure the file descriptor really is a cgroup fd and is on the
	 * v2 hierarchy.
	 */
	cgrp = cgroup_get_from_fd(req->cgroup_fd);
	if (IS_ERR(cgrp)) {
		DRM_DEBUG_DRIVER("Invalid cgroup file descriptor\n");
		return PTR_ERR(cgrp);
	}

	/*
	 * Access control:  The strategy for using cgroups in a given
	 * environment is generally determined by the system integrator
	 * and/or OS vendor, so the specific policy about who can/can't
	 * manipulate them tends to be domain-specific (and may vary
	 * depending on the location in the cgroup hierarchy).  Rather than
	 * trying to tie permission on this ioctl to a DRM-specific concepts
	 * like DRM master, we'll allow cgroup parameters to be set by any
	 * process that has been granted write access on the cgroup's
	 * virtual file system (i.e., the same permissions that would
	 * generally be needed to update the virtual files provided by
	 * cgroup controllers).
	 */
	f = fget_raw(req->cgroup_fd);
	if (WARN_ON(!f))
		return -EBADF;

	inode = kernfs_get_inode(f->f_path.dentry->d_sb, cgrp->kn);
	if (inode)
		ret = inode_permission(inode, MAY_WRITE);
	else
		ret = -ENOMEM;

	iput(inode);
	fput(f);

	if (ret)
		return ret;

	switch (req->param) {
	case I915_CGROUP_PARAM_PRIORITY_OFFSET:
		cgrpdata = cgroup_driver_get_data(dev_priv->i915_cgroups, cgrp,
						  NULL);
		if (IS_ERR(cgrpdata))
			return PTR_ERR(cgrpdata);

		DRM_DEBUG_DRIVER("Setting cgroup priority offset to %lld\n",
				 req->value);
		i915data = cgrp_to_i915(cgrpdata);
		i915data->priority_offset = req->value;
		break;
	default:
		DRM_DEBUG_DRIVER("Invalid cgroup parameter %lld\n", req->param);
		return -EINVAL;
	}

	return 0;
}

/**
 * i915_cgroup_get_prio_offset() - get prio offset for current proc's cgroup
 * @dev_priv: drm device
 * @file_priv: file handle for calling process
 *
 * RETURNS:
 * Priority offset associated with the calling process' cgroup in the default
 * (v2) hierarchy, otherwise 0 if no explicit priority has been assigned.
 */
int
i915_cgroup_get_prio_offset(struct drm_i915_private *dev_priv,
			    struct drm_i915_file_private *file_priv)
{
       struct cgroup *cgrp;
       struct cgroup_driver_data *cgrpdata;

       /* Ignore internally-created contexts not associated with a process */
       if (!file_priv)
               return 0;

       cgrp = drm_file_get_cgroup(file_priv->file);
       if (WARN_ON(!cgrp))
               return 0;

       cgrpdata = cgroup_driver_get_data(dev_priv->i915_cgroups, cgrp, NULL);
       if (IS_ERR(cgrpdata))
	       return 0;

       return cgrp_to_i915(cgrpdata)->priority_offset;
}
