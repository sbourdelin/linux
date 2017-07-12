/*
 * drm kms/fb shmem (shared memory) helper functions
 *
 * Copyright (C) 2017 Noralf Trønnes
 *
 * Based on drm_fb_cma_helper.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>

#include <drm/drmP.h>
#include <drm/drm_fb_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_fb_gem_helper.h>
#include <drm/drm_fb_shmem_helper.h>

#ifdef CONFIG_DEBUG_FS
static void drm_fb_shmem_describe(struct drm_framebuffer *fb, struct seq_file *m)
{
	struct drm_fb_gem *fb_gem = to_fb_gem(fb);
	struct drm_gem_shmem_object *obj;
	int i;

	seq_printf(m, "[FB:%d] %dx%d@%4.4s\n", fb->base.id, fb->width, fb->height,
			(char *)&fb->format->format);

	for (i = 0; i < fb->format->num_planes; i++) {
		obj = to_drm_gem_shmem_obj(fb_gem->obj[i]);
		seq_printf(m, "   %d: offset=%d pitch=%d, obj: ",
				i, fb->offsets[i], fb->pitches[i]);
		drm_gem_shmem_describe(obj, m);
	}
}

/**
 * drm_fb_shmem_debugfs_show() - Helper to list shmem framebuffer objects
 *                               in debugfs.
 * @m: output file
 * @arg: private data for the callback
 */
int drm_fb_shmem_debugfs_show(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *)m->private;
	struct drm_device *dev = node->minor->dev;
	struct drm_framebuffer *fb;

	mutex_lock(&dev->mode_config.fb_lock);
	drm_for_each_fb(fb, dev)
		drm_fb_shmem_describe(fb, m);
	mutex_unlock(&dev->mode_config.fb_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(drm_fb_shmem_debugfs_show);
#endif

static int drm_fb_shmem_mmap(struct fb_info *fbi, struct vm_area_struct *vma)
{
	struct drm_fb_helper *helper = fbi->par;
	struct drm_fb_gem *fb_gem = to_fb_gem(helper->fb);

	return drm_gem_shmem_prime_mmap(fb_gem->obj[0], vma);
}

static int drm_fb_helper_fb_open(struct fb_info *fbi, int user)
{
	struct drm_fb_helper *helper = fbi->par;

	if (!try_module_get(helper->dev->driver->fops->owner))
		return -ENODEV;

	return 0;
}

static int drm_fb_helper_fb_release(struct fb_info *fbi, int user)
{
	struct drm_fb_helper *helper = fbi->par;

	module_put(helper->dev->driver->fops->owner);

	return 0;
}

static struct fb_ops drm_fb_helper_fb_ops = {
	.owner		= THIS_MODULE,
	DRM_FB_HELPER_DEFAULT_OPS,
	.fb_open	= drm_fb_helper_fb_open,
	.fb_release	= drm_fb_helper_fb_release,
	.fb_read	= drm_fb_helper_sys_read,
	.fb_write	= drm_fb_helper_sys_write,
	.fb_fillrect	= drm_fb_helper_sys_fillrect,
	.fb_copyarea	= drm_fb_helper_sys_copyarea,
	.fb_imageblit	= drm_fb_helper_sys_imageblit,
	.fb_mmap	= drm_fb_shmem_mmap,
};

/**
 * drm_fb_shmem_fbdev_probe -
 * @helper: fbdev emulation structure
 * @sizes: fbdev description
 * @fb_funcs: Framebuffer helper functions
 *
 * Drivers can use this in their &drm_fb_helper_funcs->fb_probe function.
 *
 * Returns:
 * 0 on success or a negative error code on failure.
 */
int drm_fb_shmem_fbdev_probe(struct drm_fb_helper *helper,
			     struct drm_fb_helper_surface_size *sizes,
			     const struct drm_framebuffer_funcs *fb_funcs)
{
	struct drm_device *dev = helper->dev;
	struct drm_mode_fb_cmd2 mode_cmd;
	struct drm_gem_shmem_object *obj;
	struct drm_gem_object *gem;
	struct drm_fb_gem *fb_gem;
	void *shadow = NULL;
	size_t size;
	int ret;

	size = drm_fb_helper_mode_cmd(&mode_cmd, sizes);

	obj = drm_gem_shmem_create(dev, size);
	if (IS_ERR(obj))
		return PTR_ERR(obj);

	gem = &obj->base;
	fb_gem = drm_fb_gem_alloc(dev, &mode_cmd, &gem, 1, fb_funcs);
	if (IS_ERR(fb_gem)) {
		dev_err(dev->dev, "Failed to allocate DRM framebuffer.\n");
		drm_gem_object_put_unlocked(&obj->base);
		return PTR_ERR(fb_gem);
	}

	ret = drm_gem_shmem_vmap(obj);
	if (ret)
		goto error;

	if (fb_funcs->dirty) {
		shadow = vzalloc(size);
		if (!shadow) {
			ret = -ENOMEM;
			goto error;
		}
		helper->defio_vaddr = obj->vaddr;
	}

	ret = drm_fb_helper_simple_fb_probe(helper, sizes, &fb_gem->base,
					    &drm_fb_helper_fb_ops,
					    shadow ? shadow : obj->vaddr, 0,
					    size);
	if (ret < 0)
		goto error;

	return 0;

error:
	vfree(shadow);
	drm_framebuffer_remove(&fb_gem->base);

	return ret;
}
EXPORT_SYMBOL(drm_fb_shmem_fbdev_probe);
