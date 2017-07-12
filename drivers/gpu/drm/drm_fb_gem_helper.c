/*
 * drm fb gem helper functions
 *
 * Copyright (C) 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/reservation.h>

#include <drm/drmP.h>
#include <drm/drm_atomic.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_fb_gem_helper.h>
#include <drm/drm_gem.h>

/**
 * drm_fb_gem_get_obj() - Get GEM object for framebuffer
 * @fb: The framebuffer
 * @plane: Which plane
 *
 * Returns the GEM object for given framebuffer.
 */
struct drm_gem_object *drm_fb_gem_get_obj(struct drm_framebuffer *fb,
					  unsigned int plane)
{
	struct drm_fb_gem *fb_gem = to_fb_gem(fb);

	if (plane >= 4)
		return NULL;

	return fb_gem->obj[plane];
}
EXPORT_SYMBOL_GPL(drm_fb_gem_get_obj);

/**
 * drm_fb_gem_alloc - Allocate GEM backed framebuffer
 * @dev: DRM device
 * @mode_cmd: metadata from the userspace fb creation request
 * @obj: GEM object nacking the framebuffer
 * @num_planes: Number of planes
 * @funcs: vtable to be used for the new framebuffer object
 *
 * Returns:
 * Allocated struct drm_fb_gem * or error encoded pointer.
 */
struct drm_fb_gem *
drm_fb_gem_alloc(struct drm_device *dev,
		 const struct drm_mode_fb_cmd2 *mode_cmd,
		 struct drm_gem_object **obj, unsigned int num_planes,
		 const struct drm_framebuffer_funcs *funcs)
{
	struct drm_fb_gem *fb_gem;
	int ret, i;

	fb_gem = kzalloc(sizeof(*fb_gem), GFP_KERNEL);
	if (!fb_gem)
		return ERR_PTR(-ENOMEM);

	drm_helper_mode_fill_fb_struct(dev, &fb_gem->base, mode_cmd);

	for (i = 0; i < num_planes; i++)
		fb_gem->obj[i] = obj[i];

	ret = drm_framebuffer_init(dev, &fb_gem->base, funcs);
	if (ret) {
		dev_err(dev->dev, "Failed to initialize framebuffer: %d\n", ret);
		kfree(fb_gem);
		return ERR_PTR(ret);
	}

	return fb_gem;
}
EXPORT_SYMBOL(drm_fb_gem_alloc);

/**
 * drm_fb_gem_destroy - Free GEM backed framebuffer
 * @fb: DRM framebuffer
 *
 * Frees a GEM backed framebuffer with it's backing buffer(s) and the structure
 * itself. Drivers can use this as their &drm_framebuffer_funcs->destroy
 * callback.
 */
void drm_fb_gem_destroy(struct drm_framebuffer *fb)
{
	struct drm_fb_gem *fb_gem = to_fb_gem(fb);
	int i;

	for (i = 0; i < 4; i++) {
		if (fb_gem->obj[i])
			drm_gem_object_put_unlocked(fb_gem->obj[i]);
	}

	drm_framebuffer_cleanup(fb);
	kfree(fb_gem);
}
EXPORT_SYMBOL(drm_fb_gem_destroy);

/**
 * drm_fb_gem_create_handle - Create handle for GEM backed framebuffer
 * @fb: DRM framebuffer
 * @file: drm file
 * @handle: handle created
 *
 * Drivers can use this as their &drm_framebuffer_funcs->create_handle
 * callback.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_fb_gem_create_handle(struct drm_framebuffer *fb, struct drm_file *file,
			     unsigned int *handle)
{
	struct drm_fb_gem *fb_gem = to_fb_gem(fb);

	return drm_gem_handle_create(file, fb_gem->obj[0], handle);
}
EXPORT_SYMBOL(drm_fb_gem_create_handle);

/**
 * drm_fb_gem_create_with_funcs() - helper function for the
 *                                  &drm_mode_config_funcs.fb_create
 *                                  callback
 * @dev: DRM device
 * @file: drm file for the ioctl call
 * @mode_cmd: metadata from the userspace fb creation request
 * @funcs: vtable to be used for the new framebuffer object
 *
 * This can be used to set &drm_framebuffer_funcs for drivers that need the
 * &drm_framebuffer_funcs.dirty callback. Use drm_fb_gem_create() if you don't
 * need to change &drm_framebuffer_funcs.
 */
struct drm_framebuffer *
drm_fb_gem_create_with_funcs(struct drm_device *dev, struct drm_file *file,
			     const struct drm_mode_fb_cmd2 *mode_cmd,
			     const struct drm_framebuffer_funcs *funcs)
{
	const struct drm_format_info *info;
	struct drm_gem_object *objs[4];
	struct drm_fb_gem *fb_gem;
	int ret, i;

	info = drm_get_format_info(dev, mode_cmd);
	if (!info)
		return ERR_PTR(-EINVAL);

	for (i = 0; i < info->num_planes; i++) {
		unsigned int width = mode_cmd->width / (i ? info->hsub : 1);
		unsigned int height = mode_cmd->height / (i ? info->vsub : 1);
		unsigned int min_size;

		objs[i] = drm_gem_object_lookup(file, mode_cmd->handles[i]);
		if (!objs[i]) {
			dev_err(dev->dev, "Failed to lookup GEM object\n");
			ret = -ENOENT;
			goto err_gem_object_put;
		}

		min_size = (height - 1) * mode_cmd->pitches[i]
			 + width * info->cpp[i]
			 + mode_cmd->offsets[i];

		if (objs[i]->size < min_size) {
			drm_gem_object_put_unlocked(objs[i]);
			ret = -EINVAL;
			goto err_gem_object_put;
		}
	}

	fb_gem = drm_fb_gem_alloc(dev, mode_cmd, objs, i, funcs);
	if (IS_ERR(fb_gem)) {
		ret = PTR_ERR(fb_gem);
		goto err_gem_object_put;
	}

	return &fb_gem->base;

err_gem_object_put:
	for (i--; i >= 0; i--)
		drm_gem_object_put_unlocked(objs[i]);

	return ERR_PTR(ret);
}
EXPORT_SYMBOL_GPL(drm_fb_gem_create_with_funcs);

static struct drm_framebuffer_funcs drm_fb_gem_fb_funcs = {
	.destroy	= drm_fb_gem_destroy,
	.create_handle	= drm_fb_gem_create_handle,
};

/**
 * drm_fb_gem_create() - &drm_mode_config_funcs.fb_create callback function
 * @dev: DRM device
 * @file: drm file for the ioctl call
 * @mode_cmd: metadata from the userspace fb creation request
 *
 * If your hardware has special alignment or pitch requirements these should be
 * checked before calling this function. Use drm_fb_gem_create_with_funcs() if
 * you need to set &drm_framebuffer_funcs.dirty.
 */
struct drm_framebuffer *
drm_fb_gem_create(struct drm_device *dev, struct drm_file *file,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	return drm_fb_gem_create_with_funcs(dev, file, mode_cmd,
					    &drm_fb_gem_fb_funcs);
}
EXPORT_SYMBOL_GPL(drm_fb_gem_create);

/**
 * drm_fb_gem_prepare_fb() - Prepare gem framebuffer
 * @plane: Which plane
 * @state: Plane state attach fence to
 *
 * This should be set as the &struct drm_plane_helper_funcs.prepare_fb hook.
 *
 * This function checks if the plane FB has an dma-buf attached, extracts
 * the exclusive fence and attaches it to plane state for the atomic helper
 * to wait on.
 *
 * There is no need for cleanup_fb for gem based framebuffer drivers.
 */
int drm_fb_gem_prepare_fb(struct drm_plane *plane,
			  struct drm_plane_state *state)
{
	struct dma_buf *dma_buf;
	struct dma_fence *fence;

	if ((plane->state->fb == state->fb) || !state->fb)
		return 0;

	dma_buf = drm_fb_gem_get_obj(state->fb, 0)->dma_buf;
	if (dma_buf) {
		fence = reservation_object_get_excl_rcu(dma_buf->resv);
		drm_atomic_set_fence_for_plane(state, fence);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(drm_fb_gem_prepare_fb);
