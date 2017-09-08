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
 * cma helper and have support for framebuffer flushing (dirty).
 * fbdev support is also included.
 *
 */

/**
 * DOC: core
 *
 * The driver allocates &tinydrm_device, initializes it using
 * devm_tinydrm_init(), sets up the pipeline using tinydrm_display_pipe_init()
 * and registers the DRM device using devm_tinydrm_register().
 *
 * Device unplug
 * -------------
 *
 * tinydrm supports device unplugging when there are still open DRM or fbdev
 * file handles.
 *
 * There are 3 ways for driver-device unbinding to happen:
 *
 * - The driver module is unloaded causing the driver to be unregistered.
 *   This can't happen as long as there are open file handles because a
 *   reference is taken on the module.
 *
 * - The device is removed (USB, Device Tree overlay).
 *   This can happen at any time.
 *
 * - The driver sysfs _unbind_ file can be used to unbind the driver from the
 *   device. This can happen any time.
 *
 * The driver needs to protect device resources from access after the device is
 * gone. This is done marking the region with drm_dev_enter() and
 * drm_dev_exit(), typically in &drm_framebuffer_funcs.dirty,
 * &drm_simple_display_pipe_funcs.enable and \.disable.
 *
 * Resources that don't face userspace and are only used with the
 * device can be setup using devm\_ functions, but &tinydrm_device must be
 * allocated using plain kzalloc() since it's lifetime can exceed that of the
 * device.
 *
 * The structure should be freed in the &drm_driver->release callback.
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
	struct tinydrm_device *tdev = drm_to_tinydrm(drm);

	DRM_DEBUG_KMS("\n");
	drm_fbdev_cma_restore_mode(tdev->fbdev_cma);
}
EXPORT_SYMBOL(tinydrm_lastclose);

/**
 * tinydrm_gem_cma_prime_import_sg_table - Produce a CMA GEM object from
 *     another driver's scatter/gather table of pinned pages
 * @drm: DRM device to import into
 * @attach: DMA-BUF attachment
 * @sgt: Scatter/gather table of pinned pages
 *
 * This function imports a scatter/gather table exported via DMA-BUF by
 * another driver using drm_gem_cma_prime_import_sg_table(). It sets the
 * kernel virtual address on the CMA object. Drivers should use this as their
 * &drm_driver->gem_prime_import_sg_table callback if they need the virtual
 * address. tinydrm_gem_cma_free_object() should be used in combination with
 * this function.
 *
 * Returns:
 * A pointer to a newly created GEM object or an ERR_PTR-encoded negative
 * error code on failure.
 */
struct drm_gem_object *
tinydrm_gem_cma_prime_import_sg_table(struct drm_device *drm,
				      struct dma_buf_attachment *attach,
				      struct sg_table *sgt)
{
	struct drm_gem_cma_object *cma_obj;
	struct drm_gem_object *obj;
	void *vaddr;

	vaddr = dma_buf_vmap(attach->dmabuf);
	if (!vaddr) {
		DRM_ERROR("Failed to vmap PRIME buffer\n");
		return ERR_PTR(-ENOMEM);
	}

	obj = drm_gem_cma_prime_import_sg_table(drm, attach, sgt);
	if (IS_ERR(obj)) {
		dma_buf_vunmap(attach->dmabuf, vaddr);
		return obj;
	}

	cma_obj = to_drm_gem_cma_obj(obj);
	cma_obj->vaddr = vaddr;

	return obj;
}
EXPORT_SYMBOL(tinydrm_gem_cma_prime_import_sg_table);

/**
 * tinydrm_gem_cma_free_object - Free resources associated with a CMA GEM
 *                               object
 * @gem_obj: GEM object to free
 *
 * This function frees the backing memory of the CMA GEM object, cleans up the
 * GEM object state and frees the memory used to store the object itself using
 * drm_gem_cma_free_object(). It also handles PRIME buffers which has the kernel
 * virtual address set by tinydrm_gem_cma_prime_import_sg_table(). Drivers
 * can use this as their &drm_driver->gem_free_object callback.
 */
void tinydrm_gem_cma_free_object(struct drm_gem_object *gem_obj)
{
	if (gem_obj->import_attach) {
		struct drm_gem_cma_object *cma_obj;

		cma_obj = to_drm_gem_cma_obj(gem_obj);
		dma_buf_vunmap(gem_obj->import_attach->dmabuf, cma_obj->vaddr);
		cma_obj->vaddr = NULL;
	}

	drm_gem_cma_free_object(gem_obj);
}
EXPORT_SYMBOL_GPL(tinydrm_gem_cma_free_object);

static struct drm_framebuffer *
tinydrm_fb_create(struct drm_device *drm, struct drm_file *file_priv,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	struct tinydrm_device *tdev = drm_to_tinydrm(drm);

	return drm_fb_cma_create_with_funcs(drm, file_priv, mode_cmd,
					    tdev->fb_funcs);
}

static const struct drm_mode_config_funcs tinydrm_mode_config_funcs = {
	.fb_create = tinydrm_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

/**
 * tinydrm_release - DRM driver release helper
 * @drm: DRM device
 *
 * This function finalizes &drm_device. The caller is responsible for freeing
 * &tinydrm_device.
 *
 * Drivers must use this in their &drm_driver->release callback.
 */
void tinydrm_release(struct drm_device *drm)
{
	struct tinydrm_device *tdev = drm_to_tinydrm(drm);

	DRM_DEBUG_DRIVER("\n");

	drm_fbdev_cma_fini(tdev->fbdev_cma);

	drm_mode_config_cleanup(&tdev->drm);
	drm_dev_fini(drm);

	mutex_destroy(&tdev->dirty_lock);
}
EXPORT_SYMBOL(tinydrm_release);

static int tinydrm_init(struct device *parent, struct tinydrm_device *tdev,
			const struct drm_framebuffer_funcs *fb_funcs,
			struct drm_driver *driver)
{
	struct drm_device *drm = &tdev->drm;
	int ret;

	mutex_init(&tdev->dirty_lock);
	tdev->fb_funcs = fb_funcs;

	ret = drm_dev_init(drm, driver, parent);
	if (ret)
		return ret;

	drm_mode_config_init(drm);
	drm->mode_config.funcs = &tinydrm_mode_config_funcs;

	return 0;
}

static void devm_tinydrm_release(void *data)
{
	struct tinydrm_device *tdev = data;

	drm_dev_unref(&tdev->drm);
}

/**
 * devm_tinydrm_init - Initialize tinydrm device
 * @parent: Parent device object
 * @tdev: tinydrm device
 * @fb_funcs: Framebuffer functions
 * @driver: DRM driver
 *
 * This function initializes @tdev, the underlying DRM device and its
 * mode_config. drm_dev_unref() is called on driver detach (devres) and when
 * all refs are dropped, the &drm_driver->release callback is called which in
 * turn calls tinydrm_release().
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
		devm_tinydrm_release(tdev);

	return ret;
}
EXPORT_SYMBOL(devm_tinydrm_init);

static int tinydrm_register(struct tinydrm_device *tdev)
{
	struct drm_device *drm = &tdev->drm;
	int bpp = drm->mode_config.preferred_depth;
	struct drm_fbdev_cma *fbdev;
	int ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	fbdev = drm_fbdev_cma_init_with_funcs(drm, bpp ? bpp : 32,
					      drm->mode_config.num_connector,
					      tdev->fb_funcs);
	if (IS_ERR(fbdev))
		DRM_ERROR("Failed to initialize fbdev: %ld\n", PTR_ERR(fbdev));
	else
		tdev->fbdev_cma = fbdev;

	return 0;
}

static void tinydrm_unregister(struct tinydrm_device *tdev)
{
	if (tdev->fbdev_cma)
		drm_fb_helper_unregister_fbi(&tdev->fbdev_cma->fb_helper);
	drm_dev_unplug(&tdev->drm);
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
	struct device *dev = tdev->drm.dev;
	int ret;

	ret = tinydrm_register(tdev);
	if (ret)
		return ret;

	ret = devm_add_action(dev, devm_tinydrm_register_release, tdev);
	if (ret) {
		devm_tinydrm_register_release(tdev);
		return ret;
	}

	/*
	 * Take a ref that will be put in devm_tinydrm_release().
	 * It's done like this so devres cleanup can happen if there's an error
	 * in the probe function between calling devm_tinydrm_init() and
	 * devm_tinydrm_register().
	 */
	drm_dev_ref(&tdev->drm);

	return 0;
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
	drm_atomic_helper_shutdown(&tdev->drm);
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

	drm_fbdev_cma_set_suspend_unlocked(tdev->fbdev_cma, 1);
	state = drm_atomic_helper_suspend(&tdev->drm);
	if (IS_ERR(state)) {
		drm_fbdev_cma_set_suspend_unlocked(tdev->fbdev_cma, 0);
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

	ret = drm_atomic_helper_resume(&tdev->drm, state);
	if (ret) {
		DRM_ERROR("Error resuming state: %d\n", ret);
		return ret;
	}

	drm_fbdev_cma_set_suspend_unlocked(tdev->fbdev_cma, 0);

	return 0;
}
EXPORT_SYMBOL(tinydrm_resume);

MODULE_LICENSE("GPL");
