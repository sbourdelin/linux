/*
 * Copyright 2017 Noralf Trønnes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/tinydrm/tinydrm-helpers.h>
#include <drm/tinydrm/tinydrm-panel.h>

/**
 * DOC: overview
 *
 * This library provides helpers for displays/panels that can be operated
 * using a simple vtable.
 *
 * Many controllers are operated through a register making &regmap a useful
 * abstraction for the register interface code. This helper is geared towards
 * such controllers. Often controllers also support more than one bus, and
 * should for instance a controller be connected in a non-standard way
 * (e.g. memory mapped), then only the regmap needs to be changed.
 */

static int tinydrm_panel_prepare(struct tinydrm_panel *panel)
{
	if (panel->funcs && panel->funcs->prepare)
		return panel->funcs->prepare(panel);

	if (panel->regulator)
		return regulator_enable(panel->regulator);

	return 0;
}

static int tinydrm_panel_enable(struct tinydrm_panel *panel)
{
	if (panel->funcs && panel->funcs->enable)
		return panel->funcs->enable(panel);

	return tinydrm_enable_backlight(panel->backlight);
}

static int tinydrm_panel_disable(struct tinydrm_panel *panel)
{
	if (panel->funcs && panel->funcs->disable)
		return panel->funcs->disable(panel);

	return tinydrm_disable_backlight(panel->backlight);
}

static int tinydrm_panel_unprepare(struct tinydrm_panel *panel)
{
	if (panel->funcs && panel->funcs->unprepare)
		return panel->funcs->unprepare(panel);

	if (panel->regulator)
		return regulator_disable(panel->regulator);

	return 0;
}

static void tinydrm_panel_pipe_enable(struct drm_simple_display_pipe *pipe,
				      struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = tinydrm_to_panel(tdev);
	struct drm_framebuffer *fb = pipe->plane.fb;

	panel->enabled = true;
	fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);
	tinydrm_panel_enable(panel);
}

static void tinydrm_panel_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = tinydrm_to_panel(tdev);

	panel->enabled = false;
	tinydrm_panel_disable(panel);
}

static void tinydrm_panel_pipe_update(struct drm_simple_display_pipe *pipe,
				      struct drm_plane_state *old_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct tinydrm_panel *panel = tinydrm_to_panel(tdev);
	struct drm_framebuffer *fb = pipe->plane.state->fb;

	/* fb is set (not changed) */
	if (fb && !old_state->fb)
		tinydrm_panel_prepare(panel);

	tinydrm_display_pipe_update(pipe, old_state);

	/* fb is unset */
	if (!fb)
		tinydrm_panel_unprepare(panel);
}

static const struct drm_simple_display_pipe_funcs tinydrm_panel_pipe_funcs = {
	.enable = tinydrm_panel_pipe_enable,
	.disable = tinydrm_panel_pipe_disable,
	.update = tinydrm_panel_pipe_update,
	.prepare_fb = tinydrm_display_pipe_prepare_fb,
};

static int tinydrm_panel_fb_dirty(struct drm_framebuffer *fb,
				  struct drm_file *file_priv,
				  unsigned int flags, unsigned int color,
				  struct drm_clip_rect *clips,
				  unsigned int num_clips)
{
	struct tinydrm_device *tdev = fb->dev->dev_private;
	struct tinydrm_panel *panel = tinydrm_to_panel(tdev);
	struct drm_clip_rect rect;
	int ret = 0;

	if (!panel->funcs || !panel->funcs->flush)
		return 0;

	mutex_lock(&tdev->dirty_lock);

	if (!panel->enabled)
		goto out_unlock;

	/* fbdev can flush even when we're not interested */
	if (tdev->pipe.plane.fb != fb)
		goto out_unlock;

	tinydrm_merge_clips(&rect, clips, num_clips, flags,
			    fb->width, fb->height);

	ret = panel->funcs->flush(panel, fb, &rect);

out_unlock:
	mutex_unlock(&tdev->dirty_lock);

	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);

	return ret;
}

static const struct drm_framebuffer_funcs tinydrm_panel_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= tinydrm_panel_fb_dirty,
};

/**
 * tinydrm_panel_init - Initialize &tinydrm_panel
 * @dev: Parent device
 * @panel: &tinydrm_panel structure to initialize
 * @funcs: Callbacks for the panel (optional)
 * @formats: Array of supported formats (DRM_FORMAT\_\*). The first format is
 *           considered the default format and &tinydrm_panel->tx_buf is
 *           allocated a buffer that can hold a framebuffer with that format.
 * @format_count: Number of elements in @formats
 * @driver: DRM driver
 * @mode: Display mode
 * @rotation: Initial rotation in degrees Counter Clock Wise
 *
 * This function initializes a &tinydrm_panel structure and it's underlying
 * @tinydrm_device. It also sets up the display pipeline.
 *
 * Objects created by this function will be automatically freed on driver
 * detach (devres).
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_panel_init(struct device *dev, struct tinydrm_panel *panel,
		       const struct tinydrm_panel_funcs *funcs,
		       const uint32_t *formats, unsigned int format_count,
		       struct drm_driver *driver,
		       const struct drm_display_mode *mode,
		       unsigned int rotation)
{
	struct tinydrm_device *tdev = &panel->tinydrm;
	const struct drm_format_info *format_info;
	size_t bufsize;
	int ret;

	format_info = drm_format_info(formats[0]);
	bufsize = mode->vdisplay * mode->hdisplay * format_info->cpp[0];

	panel->tx_buf = devm_kmalloc(dev, bufsize, GFP_KERNEL);
	if (!panel->tx_buf)
		return -ENOMEM;

	ret = devm_tinydrm_init(dev, tdev, &tinydrm_panel_fb_funcs, driver);
	if (ret)
		return ret;

	ret = tinydrm_display_pipe_init(tdev, &tinydrm_panel_pipe_funcs,
					DRM_MODE_CONNECTOR_VIRTUAL,
					formats, format_count, mode,
					rotation);
	if (ret)
		return ret;

	tdev->drm->mode_config.preferred_depth = format_info->depth;

	panel->rotation = rotation;
	panel->funcs = funcs;

	drm_mode_config_reset(tdev->drm);

	DRM_DEBUG_KMS("preferred_depth=%u, rotation = %u\n",
		      tdev->drm->mode_config.preferred_depth, rotation);

	return 0;
}
EXPORT_SYMBOL(tinydrm_panel_init);

/**
 * tinydrm_panel_rgb565_buf - Return RGB565 buffer to scanout
 * @panel: tinydrm panel
 * @fb: DRM framebuffer
 * @rect: Clip rectangle area to scanout
 *
 * This function returns the RGB565 framebuffer rectangle to scanout.
 * It converts XRGB8888 to RGB565 if necessary.
 * If copying isn't necessary (RGB565 full rect, no swap), then the backing
 * CMA buffer is returned.
 *
 * Returns:
 * Buffer to scanout on success, ERR_PTR on failure.
 */
void *tinydrm_panel_rgb565_buf(struct tinydrm_panel *panel,
			       struct drm_framebuffer *fb,
			       struct drm_clip_rect *rect)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	bool swap = panel->swap_bytes;
	bool full;
	void *buf;
	int ret;

	full = (rect->x2 - rect->x1) == fb->width &&
	       (rect->y2 - rect->y1) == fb->height;

	if (panel->always_tx_buf || swap || !full ||
	    fb->format->format == DRM_FORMAT_XRGB8888) {
		buf = panel->tx_buf;
		ret = tinydrm_rgb565_buf_copy(buf, fb, rect, swap);
		if (ret)
			return ERR_PTR(ret);
	} else {
		buf = cma_obj->vaddr;
	}

	return buf;
}
EXPORT_SYMBOL(tinydrm_panel_rgb565_buf);

/**
 * tinydrm_panel_pm_suspend - tinydrm_panel PM suspend helper
 * @dev: Device
 *
 * tinydrm_panel drivers can use this in their &device_driver->pm operations.
 * Use dev_set_drvdata() or similar to set &tinydrm_panel as driver data.
 */
int tinydrm_panel_pm_suspend(struct device *dev)
{
	struct tinydrm_panel *panel = dev_get_drvdata(dev);
	int ret;

	ret = tinydrm_suspend(&panel->tinydrm);
	if (ret)
		return ret;

	/* fb isn't set to NULL by suspend, do .unprepare() explicitly */
	tinydrm_panel_unprepare(panel);

	return 0;
}
EXPORT_SYMBOL(tinydrm_panel_pm_suspend);

/**
 * tinydrm_panel_pm_resume - tinydrm_panel PM resume helper
 * @dev: Device
 *
 * tinydrm_panel drivers can use this in their &device_driver->pm operations.
 * Use dev_set_drvdata() or similar to set &tinydrm_panel as driver data.
 */
int tinydrm_panel_pm_resume(struct device *dev)
{
	struct tinydrm_panel *panel = dev_get_drvdata(dev);

	/* fb is NULL on resume, .prepare() will be called in pipe_update */

	return tinydrm_resume(&panel->tinydrm);
}
EXPORT_SYMBOL(tinydrm_panel_pm_resume);

/**
 * tinydrm_panel_spi_shutdown - tinydrm_panel SPI shutdown helper
 * @spi: SPI device
 *
 * tinydrm_panel drivers can use this as their shutdown callback to turn off
 * the display on machine shutdown and reboot. Use spi_set_drvdata() or
 * similar to set &tinydrm_panel as driver data.
 */
void tinydrm_panel_spi_shutdown(struct spi_device *spi)
{
	struct tinydrm_panel *panel = spi_get_drvdata(spi);

	tinydrm_shutdown(&panel->tinydrm);
}
EXPORT_SYMBOL(tinydrm_panel_spi_shutdown);

/**
 * tinydrm_panel_i2c_shutdown - tinydrm_panel I2C shutdown helper
 * @i2c: I2C client device
 *
 * tinydrm_panel drivers can use this as their shutdown callback to turn off
 * the display on machine shutdown and reboot. Use i2c_set_clientdata() or
 * similar to set &tinydrm_panel as driver data.
 */
void tinydrm_panel_i2c_shutdown(struct i2c_client *i2c)
{
	struct tinydrm_panel *panel = i2c_get_clientdata(i2c);

	tinydrm_shutdown(&panel->tinydrm);
}
EXPORT_SYMBOL(tinydrm_panel_i2c_shutdown);

/**
 * tinydrm_panel_platform_shutdown - tinydrm_panel platform driver shutdown
 *                                   helper
 * @pdev: Platform device
 *
 * tinydrm_panel drivers can use this as their shutdown callback to turn off
 * the display on machine shutdown and reboot. Use platform_set_drvdata() or
 * similar to set &tinydrm_panel as driver data.
 */
void tinydrm_panel_platform_shutdown(struct platform_device *pdev)
{
	struct tinydrm_panel *panel = platform_get_drvdata(pdev);

	tinydrm_shutdown(&panel->tinydrm);
}
EXPORT_SYMBOL(tinydrm_panel_platform_shutdown);

/**
 * tinydrm_regmap_raw_swap_bytes - Does a raw write require swapping bytes?
 * @reg: Regmap
 *
 * If the bus doesn't support the full regwidth, it has to break up the word.
 * Additionally if the bus and machine doesn't match endian wise, this requires
 * byteswapping the buffer when using regmap_raw_write().
 *
 * Returns:
 * True if byte swapping is needed, otherwise false
 */
bool tinydrm_regmap_raw_swap_bytes(struct regmap *reg)
{
	int val_bytes = regmap_get_val_bytes(reg);
	unsigned int bus_val;
	u16 val16 = 0x00ff;

	if (val_bytes == 1)
		return false;

	if (WARN_ON_ONCE(val_bytes != 2))
		return false;

	regmap_parse_val(reg, &val16, &bus_val);

	return val16 != bus_val;
}
EXPORT_SYMBOL(tinydrm_regmap_raw_swap_bytes);

#ifdef CONFIG_DEBUG_FS

static int
tinydrm_kstrtoul_array_from_user(const char __user *s, size_t count,
				 unsigned int base,
				 unsigned long *vals, size_t num_vals)
{
	char *buf, *pos, *token;
	int ret, i = 0;

	buf = memdup_user_nul(s, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	pos = buf;
	while (pos) {
		if (i == num_vals) {
			ret = -E2BIG;
			goto err_free;
		}

		token = strsep(&pos, " ");
		if (!token) {
			ret = -EINVAL;
			goto err_free;
		}

		ret = kstrtoul(token, base, vals++);
		if (ret < 0)
			goto err_free;
		i++;
	}

err_free:
	kfree(buf);

	return ret ? ret : i;
}

static ssize_t tinydrm_regmap_debugfs_reg_write(struct file *file,
						const char __user *user_buf,
						size_t count, loff_t *ppos)
{
	struct seq_file *m = file->private_data;
	struct regmap *reg = m->private;
	unsigned long vals[2];
	int ret;

	ret = tinydrm_kstrtoul_array_from_user(user_buf, count, 16, vals, 2);
	if (ret <= 0)
		return ret;

	if (ret != 2)
		return -EINVAL;

	ret = regmap_write(reg, vals[0], vals[1]);

	return ret < 0 ? ret : count;
}

static int tinydrm_regmap_debugfs_reg_show(struct seq_file *m, void *d)
{
	struct regmap *reg = m->private;
	int max_reg = regmap_get_max_register(reg);
	int val_bytes = regmap_get_val_bytes(reg);
	unsigned int val;
	int regnr, ret;

	for (regnr = 0; regnr < max_reg; regnr++) {
		seq_printf(m, "%.*x: ", val_bytes * 2, regnr);
		ret = regmap_read(reg, regnr, &val);
		if (ret)
			seq_puts(m, "XX\n");
		else
			seq_printf(m, "%.*x\n", val_bytes * 2, val);
	}

	return 0;
}

static int tinydrm_regmap_debugfs_reg_open(struct inode *inode,
					   struct file *file)
{
	return single_open(file, tinydrm_regmap_debugfs_reg_show,
			   inode->i_private);
}

static const struct file_operations tinydrm_regmap_debugfs_reg_fops = {
	.owner = THIS_MODULE,
	.open = tinydrm_regmap_debugfs_reg_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = tinydrm_regmap_debugfs_reg_write,
};

static int
tinydrm_regmap_debugfs_init(struct regmap *reg, struct dentry *parent)
{
	umode_t mode = 0200;

	if (regmap_get_max_register(reg))
		mode |= 0444;

	debugfs_create_file("registers", mode, parent, reg,
			    &tinydrm_regmap_debugfs_reg_fops);
	return 0;
}

static const struct drm_info_list tinydrm_panel_debugfslist[] = {
	{ "fb",   drm_fb_cma_debugfs_show, 0 },
};

/**
 * tinydrm_panel_debugfs_init - Create tinydrm panel debugfs entries
 * @minor: DRM minor
 *
 * &tinydrm_panel drivers can use this as their
 * &drm_driver->debugfs_init callback.
 *
 * Returns:
 * Zero on success, negative error code on failure.
 */
int tinydrm_panel_debugfs_init(struct drm_minor *minor)
{
	struct tinydrm_device *tdev = minor->dev->dev_private;
	struct tinydrm_panel *panel = tinydrm_to_panel(tdev);
	struct regmap *reg = panel->reg;
	int ret;

	if (reg) {
		ret = tinydrm_regmap_debugfs_init(reg, minor->debugfs_root);
		if (ret)
			return ret;
	}

	return drm_debugfs_create_files(tinydrm_panel_debugfslist,
					ARRAY_SIZE(tinydrm_panel_debugfslist),
					minor->debugfs_root, minor);
}
EXPORT_SYMBOL(tinydrm_panel_debugfs_init);

#endif
