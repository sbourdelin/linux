/*
 * Copyright (C) 2016 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_helper.h>
#include <drm/tinydrm/tinydrm.h>
#include <linux/device.h>
#include <linux/dma-buf.h>

/**
 * DOC: overview
 *
 * This library provides driver helpers for very simple display hardware.
 *
 * It is based on &drm_simple_display_pipe coupled with a &drm_connector which
 * has only one fixed &drm_display_mode. The framebuffers are backed by the
 * shmem helper and have support for framebuffer flushing (dirty).
 * fbdev support is also included.
 *
 */

/**
 * DOC: core
 *
 * The driver allocates &tinydrm_device, initializes it using
 * devm_tinydrm_init(), sets up the pipeline using tinydrm_display_pipe_init()
 * and registers the DRM device using devm_tinydrm_register().
 */

/**
 * tinydrm_lastclose - DRM lastclose helper
 * @drm: DRM device
 *
 * This function ensures that fbdev is restored when drm_lastclose() is called
 * on the last drm_release(). Drivers can use this as their
 * &drm_driver->lastclose callback.
 */
void tinydrm_lastclose(struct drm_device *drm)
{
	struct tinydrm_device *tdev = drm->dev_private;

	DRM_DEBUG_KMS("\n");
	if (tdev->fbdev)
		drm_fb_helper_restore_fbdev_mode_unlocked(tdev->fbdev);
}
EXPORT_SYMBOL(tinydrm_lastclose);

/**
 * tinydrm_gem_create_object - Create shmem GEM object
 * @drm: DRM device
 * @size: Size
 *
 * This function set cache mode to cached. Drivers should use this as their
 * &drm_driver->gem_create_object callback.
 */
struct drm_gem_object *tinydrm_gem_create_object(struct drm_device *drm,
						 size_t size)
{
	struct drm_gem_shmem_object *obj;

	obj = kzalloc(sizeof(*obj), GFP_KERNEL);
	if (!obj)
		return ERR_PTR(-ENOMEM);

	obj->cache_mode = DRM_GEM_SHMEM_BO_CACHED;

	return &obj->base;
}
EXPORT_SYMBOL(tinydrm_gem_create_object);

static struct drm_framebuffer *
tinydrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct tinydrm_device *tdev = drm->dev_private;

	return drm_fb_gem_create_with_funcs(drm, file_priv, mode_cmd,
					    tdev->fb_funcs);
}

static const struct drm_mode_config_funcs tinydrm_mode_config_funcs = {
	.fb_create = tinydrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int tinydrm_init(struct device *parent, struct tinydrm_device *tdev,
			const struct drm_framebuffer_funcs *fb_funcs,
			struct drm_driver *driver)
{
	struct drm_device *drm;

	mutex_init(&tdev->dirty_lock);
	tdev->fb_funcs = fb_funcs;

	/*
	 * We don't embed drm_device, because that prevent us from using
	 * devm_kzalloc() to allocate tinydrm_device in the driver since
	 * drm_dev_unref() frees the structure. The devm_ functions provide
	 * for easy error handling.
	 */
	drm = drm_dev_alloc(driver, parent);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	tdev->drm = drm;
	drm->dev_private = tdev;
	drm_mode_config_init(drm);
	drm->mode_config.funcs = &tinydrm_mode_config_funcs;

	return 0;
}

static void tinydrm_fini(struct tinydrm_device *tdev)
{
	drm_mode_config_cleanup(tdev->drm);
	mutex_destroy(&tdev->dirty_lock);
	tdev->drm->dev_private = NULL;
	drm_dev_unref(tdev->drm);
}

static void devm_tinydrm_release(void *data)
{
	tinydrm_fini(data);
}

/**
 * devm_tinydrm_init - Initialize tinydrm device
 * @parent: Parent device object
 * @tdev: tinydrm device
 * @fb_funcs: Framebuffer functions
 * @driver: DRM driver
 *
 * This function initializes @tdev, the underlying DRM device and it's
 * mode_config. Resources will be automatically freed on driver detach (devres)
 * using drm_mode_config_cleanup() and drm_dev_unref().
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int devm_tinydrm_init(struct device *parent, struct tinydrm_device *tdev,
		      const struct drm_framebuffer_funcs *fb_funcs,
		      struct drm_driver *driver)
{
	int ret;

	ret = tinydrm_init(parent, tdev, fb_funcs, driver);
	if (ret)
		return ret;

	ret = devm_add_action(parent, devm_tinydrm_release, tdev);
	if (ret)
		tinydrm_fini(tdev);

	return ret;
}
EXPORT_SYMBOL(devm_tinydrm_init);

static int tinydrm_fbdev_probe(struct drm_fb_helper *helper,
			       struct drm_fb_helper_surface_size *sizes)
{
	struct tinydrm_device *tdev = helper->dev->dev_private;

	return drm_fb_shmem_fbdev_probe(helper, sizes, tdev->fb_funcs);
}

static const struct drm_fb_helper_funcs tinydrm_fb_helper_funcs = {
	.fb_probe = tinydrm_fbdev_probe,
};

static int tinydrm_fbdev_init(struct tinydrm_device *tdev)
{
	struct drm_device *drm = tdev->drm;
	int bpp = drm->mode_config.preferred_depth;
	int ret;

	tdev->fbdev = kzalloc(sizeof(*tdev->fbdev), GFP_KERNEL);
	if (!tdev->fbdev)
		return -ENOMEM;

	ret = drm_fb_helper_simple_init(drm, tdev->fbdev, bpp ? bpp : 32,
					drm->mode_config.num_connector,
					&tinydrm_fb_helper_funcs);
	if (ret) {
		kfree(tdev->fbdev);
		tdev->fbdev = NULL;
		return ret;
	}

	return 0;
}

static int tinydrm_register(struct tinydrm_device *tdev)
{
	int ret;

	ret = drm_dev_register(tdev->drm, 0);
	if (ret)
		return ret;

	if (tinydrm_fbdev_init(tdev))
		DRM_WARN("Failed to initialize fbdev\n");

	return ret;
}

static void tinydrm_unregister(struct tinydrm_device *tdev)
{
	struct drm_fb_helper *fbdev = tdev->fbdev;

	/* don't restore fbdev in lastclose, keep pipeline disabled */
	tdev->fbdev = NULL;
	drm_atomic_helper_shutdown(tdev->drm);
	drm_fb_helper_simple_fini(fbdev);
	drm_dev_unregister(tdev->drm);
	kfree(fbdev);
}

static void devm_tinydrm_register_release(void *data)
{
	tinydrm_unregister(data);
}

/**
 * devm_tinydrm_register - Register tinydrm device
 * @tdev: tinydrm device
 *
 * This function registers the underlying DRM device and fbdev.
 * These resources will be automatically unregistered on driver detach (devres)
 * and the display pipeline will be disabled.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int devm_tinydrm_register(struct tinydrm_device *tdev)
{
	struct device *dev = tdev->drm->dev;
	int ret;

	ret = tinydrm_register(tdev);
	if (ret)
		return ret;

	ret = devm_add_action(dev, devm_tinydrm_register_release, tdev);
	if (ret)
		tinydrm_unregister(tdev);

	return ret;
}
EXPORT_SYMBOL(devm_tinydrm_register);

/**
 * tinydrm_shutdown - Shutdown tinydrm
 * @tdev: tinydrm device
 *
 * This function makes sure that the display pipeline is disabled.
 * Used by drivers in their shutdown callback to turn off the display
 * on machine shutdown and reboot.
 */
void tinydrm_shutdown(struct tinydrm_device *tdev)
{
	drm_atomic_helper_shutdown(tdev->drm);
}
EXPORT_SYMBOL(tinydrm_shutdown);

/**
 * tinydrm_suspend - Suspend tinydrm
 * @tdev: tinydrm device
 *
 * Used in driver PM operations to suspend tinydrm.
 * Suspends fbdev and DRM.
 * Resume with tinydrm_resume().
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_suspend(struct tinydrm_device *tdev)
{
	struct drm_atomic_state *state;

	if (tdev->suspend_state) {
		DRM_ERROR("Failed to suspend: state already set\n");
		return -EINVAL;
	}

	if (tdev->fbdev)
		drm_fb_helper_set_suspend_unlocked(tdev->fbdev, 1);
	state = drm_atomic_helper_suspend(tdev->drm);
	if (IS_ERR(state)) {
		if (tdev->fbdev)
			drm_fb_helper_set_suspend_unlocked(tdev->fbdev, 0);
		return PTR_ERR(state);
	}

	tdev->suspend_state = state;

	return 0;
}
EXPORT_SYMBOL(tinydrm_suspend);

/**
 * tinydrm_resume - Resume tinydrm
 * @tdev: tinydrm device
 *
 * Used in driver PM operations to resume tinydrm.
 * Suspend with tinydrm_suspend().
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_resume(struct tinydrm_device *tdev)
{
	struct drm_atomic_state *state = tdev->suspend_state;
	int ret;

	if (!state) {
		DRM_ERROR("Failed to resume: state is not set\n");
		return -EINVAL;
	}

	tdev->suspend_state = NULL;

	ret = drm_atomic_helper_resume(tdev->drm, state);
	if (ret) {
		DRM_ERROR("Error resuming state: %d\n", ret);
		return ret;
	}

	if (tdev->fbdev)
		drm_fb_helper_set_suspend_unlocked(tdev->fbdev, 0);

	return 0;
}
EXPORT_SYMBOL(tinydrm_resume);

MODULE_LICENSE("GPL");
