/*
 * DRM driver for Sitronix ST7586 panels
 *
 * Copyright 2017 David Lechner <david@lechnology.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <video/mipi_display.h>

#include <drm/tinydrm/mipi-dbi.h>
#include <drm/tinydrm/tinydrm-helpers.h>

/* controller-specific commands */
#define ST7586_DISP_MODE_GRAY	0x38
#define ST7586_DISP_MODE_MONO	0x39
#define ST7586_ENABLE_DDRAM	0x3a
#define ST7586_SET_DISP_DUTY	0xb0
#define ST7586_SET_PART_DISP	0xb4
#define ST7586_SET_NLINE_INV	0xb5
#define ST7586_SET_VOP		0xc0
#define ST7586_SET_BIAS_SYSTEM	0xc3
#define ST7586_SET_BOOST_LEVEL	0xc4
#define ST7586_SET_VOP_OFFSET	0xc7
#define ST7586_ENABLE_ANALOG	0xd0
#define ST7586_AUTO_READ_CTRL	0xd7
#define ST7586_OTP_RW_CTRL	0xe0
#define ST7586_OTP_CTRL_OUT	0xe1
#define ST7586_OTP_READ		0xe3

#define ST7586_DISP_CTRL_MX	BIT(6)
#define ST7586_DISP_CTRL_MY	BIT(7)

static void st7586_xrgb8888_to_gray332(u8 *dst, void *vaddr,
				       struct drm_framebuffer *fb,
				       struct drm_clip_rect *clip)
{
	size_t len = (clip->x2 - clip->x1) * (clip->y2 - clip->y1);
	unsigned int x, y;
	u8 *src, *buf, val;

	/* 3 pixels per byte, so grow clip to nearest multiple of 3 */
	clip->x1 = clip->x1 / 3 * 3;
	clip->x2 = (clip->x2 + 2) / 3 * 3;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return;

	tinydrm_xrgb8888_to_gray8(buf, vaddr, fb, clip);
	src = buf;

	for (y = clip->y1; y < clip->y2; y++) {
		for (x = clip->x1; x < clip->x2; x += 3) {
			val = *src++ & 0xc0;
			if (val & 0xc0)
				val |= 0x20;
			val |= (*src++ & 0xc0) >> 3;
			if (val & 0x18)
				val |= 0x04;
			val |= *src++ >> 6;
			*dst++ = ~val;
		}
	}

	/* now adjust the clip so it applies to dst */
	clip->x1 /= 3;
	clip->x2 /= 3;

	kfree(buf);
}

static int st7586_buf_copy(void *dst, struct drm_framebuffer *fb,
			   struct drm_clip_rect *clip)
{
	struct drm_gem_cma_object *cma_obj = drm_fb_cma_get_gem_obj(fb, 0);
	struct dma_buf_attachment *import_attach = cma_obj->base.import_attach;
	struct drm_format_name_buf format_name;
	void *src = cma_obj->vaddr;
	int ret = 0;

	if (import_attach) {
		ret = dma_buf_begin_cpu_access(import_attach->dmabuf,
					       DMA_FROM_DEVICE);
		if (ret)
			return ret;
	}

	switch (fb->format->format) {
	case DRM_FORMAT_XRGB8888:
		st7586_xrgb8888_to_gray332(dst, src, fb, clip);
		break;
	default:
		dev_err_once(fb->dev->dev, "Format is not supported: %s\n",
			     drm_get_format_name(fb->format->format,
						 &format_name));
		ret = -EINVAL;
		break;
	}

	if (import_attach)
		dma_buf_end_cpu_access(import_attach->dmabuf, DMA_FROM_DEVICE);

	return ret;
}

static int st7586_fb_dirty(struct drm_framebuffer *fb,
			   struct drm_file *file_priv, unsigned int flags,
			   unsigned int color, struct drm_clip_rect *clips,
			   unsigned int num_clips)
{
	struct tinydrm_device *tdev = fb->dev->dev_private;
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct drm_clip_rect clip;
	int ret = 0;

	mutex_lock(&tdev->dirty_lock);

	if (!mipi->enabled)
		goto out_unlock;

	/* fbdev can flush even when we're not interested */
	if (tdev->pipe.plane.fb != fb)
		goto out_unlock;

	tinydrm_merge_clips(&clip, clips, num_clips, flags, fb->width,
			    fb->height);

	DRM_DEBUG("Flushing [FB:%d] x1=%u, x2=%u, y1=%u, y2=%u\n", fb->base.id,
		  clip.x1, clip.x2, clip.y1, clip.y2);

	ret = st7586_buf_copy(mipi->tx_buf, fb, &clip);
	if (ret)
		goto out_unlock;

	/* NB: st7586_buf_copy() modifies clip */

	mipi_dbi_command(mipi, MIPI_DCS_SET_COLUMN_ADDRESS,
			 (clip.x1 >> 8) & 0xFF, clip.x1 & 0xFF,
			 (clip.x2 >> 8) & 0xFF, (clip.x2 - 1) & 0xFF);
	mipi_dbi_command(mipi, MIPI_DCS_SET_PAGE_ADDRESS,
			 (clip.y1 >> 8) & 0xFF, clip.y1 & 0xFF,
			 (clip.y2 >> 8) & 0xFF, (clip.y2 - 1) & 0xFF);

	ret = mipi_dbi_command_buf(mipi, MIPI_DCS_WRITE_MEMORY_START,
				   (u8 *)mipi->tx_buf,
				   (clip.x2 - clip.x1) * (clip.y2 - clip.y1));

out_unlock:
	mutex_unlock(&tdev->dirty_lock);

	if (ret)
		dev_err_once(fb->dev->dev, "Failed to update display %d\n",
			     ret);

	return ret;
}

static const struct drm_framebuffer_funcs st7586_fb_funcs = {
	.destroy	= drm_fb_cma_destroy,
	.create_handle	= drm_fb_cma_create_handle,
	.dirty		= st7586_fb_dirty,
};

void st7586_pipe_enable(struct drm_simple_display_pipe *pipe,
			struct drm_crtc_state *crtc_state)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);
	struct drm_framebuffer *fb = pipe->plane.fb;
	struct device *dev = tdev->drm->dev;
	int ret;
	u8 addr_mode;

	DRM_DEBUG_KMS("\n");

	ret = regulator_enable(mipi->regulator);
	if (ret) {
		dev_err(dev, "Failed to enable regulator %d\n", ret);
		return;
	}

	mipi_dbi_hw_reset(mipi);
	ret = mipi_dbi_command(mipi, ST7586_AUTO_READ_CTRL, 0x9f);
	if (ret) {
		dev_err(dev, "Error sending command %d\n", ret);
		regulator_disable(mipi->regulator);
		return;
	}

	mipi_dbi_command(mipi, ST7586_OTP_RW_CTRL, 0x00);

	msleep(10);

	mipi_dbi_command(mipi, ST7586_OTP_READ);

	msleep(20);

	mipi_dbi_command(mipi, ST7586_OTP_CTRL_OUT);
	mipi_dbi_command(mipi, MIPI_DCS_EXIT_SLEEP_MODE);
	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_OFF);

	msleep(50);

	mipi_dbi_command(mipi, ST7586_SET_VOP_OFFSET, 0x00);
	mipi_dbi_command(mipi, ST7586_SET_VOP, 0xe3, 0x00);
	mipi_dbi_command(mipi, ST7586_SET_BIAS_SYSTEM, 0x02);
	mipi_dbi_command(mipi, ST7586_SET_BOOST_LEVEL, 0x04);
	mipi_dbi_command(mipi, ST7586_ENABLE_ANALOG, 0x1d);
	mipi_dbi_command(mipi, ST7586_SET_NLINE_INV, 0x00);
	mipi_dbi_command(mipi, ST7586_DISP_MODE_GRAY);
	mipi_dbi_command(mipi, ST7586_ENABLE_DDRAM, 0x02);

	switch (mipi->rotation) {
	default:
		addr_mode = 0x00;
		break;
	case 90:
		addr_mode = ST7586_DISP_CTRL_MY;
		break;
	case 180:
		addr_mode = ST7586_DISP_CTRL_MX | ST7586_DISP_CTRL_MY;
		break;
	case 270:
		addr_mode = ST7586_DISP_CTRL_MX;
		break;
	}
	mipi_dbi_command(mipi, MIPI_DCS_SET_ADDRESS_MODE, addr_mode);

	mipi_dbi_command(mipi, ST7586_SET_DISP_DUTY, 0x7f);
	mipi_dbi_command(mipi, ST7586_SET_PART_DISP, 0xa0);
	mipi_dbi_command(mipi, MIPI_DCS_SET_PARTIAL_AREA, 0x00, 0x00, 0x00, 0x77);
	mipi_dbi_command(mipi, MIPI_DCS_EXIT_INVERT_MODE);

	msleep(100);

	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_ON);

	mipi->enabled = true;

	if (fb)
		fb->funcs->dirty(fb, NULL, 0, 0, NULL, 0);

	tinydrm_enable_backlight(mipi->backlight);
}

static void st7586_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct tinydrm_device *tdev = pipe_to_tinydrm(pipe);
	struct mipi_dbi *mipi = mipi_dbi_from_tinydrm(tdev);

	DRM_DEBUG_KMS("\n");

	if (!mipi->enabled)
		return;

	mipi->enabled = false;
	tinydrm_disable_backlight(mipi->backlight);
	mipi_dbi_command(mipi, MIPI_DCS_SET_DISPLAY_OFF);
	regulator_disable(mipi->regulator);
}

static const u32 st7586_formats[] = {
	DRM_FORMAT_XRGB8888,
};

static int st7586_init(struct device *dev, struct mipi_dbi *mipi,
		const struct drm_simple_display_pipe_funcs *pipe_funcs,
		struct drm_driver *driver, const struct drm_display_mode *mode,
		unsigned int rotation)
{
	size_t bufsize = (mode->vdisplay + 2) / 3 * mode->hdisplay;
	struct tinydrm_device *tdev = &mipi->tinydrm;
	int ret;

	mutex_init(&mipi->cmdlock);

	mipi->tx_buf = devm_kmalloc(dev, bufsize, GFP_KERNEL);
	if (!mipi->tx_buf)
		return -ENOMEM;

	ret = devm_tinydrm_init(dev, tdev, &st7586_fb_funcs, driver);
	if (ret)
		return ret;

	ret = tinydrm_display_pipe_init(tdev, pipe_funcs,
					DRM_MODE_CONNECTOR_VIRTUAL,
					st7586_formats,
					ARRAY_SIZE(st7586_formats),
					mode, rotation);
	if (ret)
		return ret;

	tdev->drm->mode_config.preferred_depth = 32;
	mipi->rotation = rotation;

	drm_mode_config_reset(tdev->drm);

	DRM_DEBUG_KMS("preferred_depth=%u, rotation = %u\n",
		      tdev->drm->mode_config.preferred_depth, rotation);

	return 0;
}

static const struct drm_simple_display_pipe_funcs st7586_pipe_funcs = {
	.enable		= st7586_pipe_enable,
	.disable	= st7586_pipe_disable,
	.update		= tinydrm_display_pipe_update,
	.prepare_fb	= tinydrm_display_pipe_prepare_fb,
};

static const struct drm_display_mode st7586_mode = {
	TINYDRM_MODE(178, 128, 37, 27),
};

DEFINE_DRM_GEM_CMA_FOPS(st7586_fops);

static struct drm_driver st7586_driver = {
	.driver_features	= DRIVER_GEM | DRIVER_MODESET | DRIVER_PRIME |
				  DRIVER_ATOMIC,
	.fops			= &st7586_fops,
	TINYDRM_GEM_DRIVER_OPS,
	.lastclose		= tinydrm_lastclose,
	.debugfs_init		= mipi_dbi_debugfs_init,
	.name			= "st7586",
	.desc			= "Sitronix ST7586",
	.date			= "20170801",
	.major			= 1,
	.minor			= 0,
};

static const struct of_device_id st7586_of_match[] = {
	{ .compatible = "lego,ev3-lcd" },
	{},
};
MODULE_DEVICE_TABLE(of, st7586_of_match);

static const struct spi_device_id st7586_id[] = {
	{ "ev3-lcd", 0 },
	{ },
};
MODULE_DEVICE_TABLE(spi, st7586_id);

static int st7586_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct tinydrm_device *tdev;
	struct mipi_dbi *mipi;
	struct gpio_desc *dc;
	u32 rotation = 0;
	int ret;

	mipi = devm_kzalloc(dev, sizeof(*mipi), GFP_KERNEL);
	if (!mipi)
		return -ENOMEM;

	mipi->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(mipi->reset)) {
		dev_err(dev, "Failed to get gpio 'reset'\n");
		return PTR_ERR(mipi->reset);
	}

	dc = devm_gpiod_get_optional(dev, "dc", GPIOD_OUT_LOW);
	if (IS_ERR(dc)) {
		dev_err(dev, "Failed to get gpio 'dc'\n");
		return PTR_ERR(dc);
	}

	mipi->regulator = devm_regulator_get(dev, "power");
	if (IS_ERR(mipi->regulator))
		return PTR_ERR(mipi->regulator);

	mipi->backlight = tinydrm_of_find_backlight(dev);
	if (IS_ERR(mipi->backlight))
		return PTR_ERR(mipi->backlight);

	device_property_read_u32(dev, "rotation", &rotation);

	ret = mipi_dbi_spi_init(spi, mipi, dc);
	if (ret)
		return ret;

	/* Cannot read from this controller via SPI */
	mipi->read_commands = NULL;

	/*
	 * we are using 8-bit data, so we are not actually swapping anything,
	 * but setting mipi->swap_bytes makes mipi_dbi_typec3_command() do the
	 * right thing and not use 16-bit transfers (which results in swapped
	 * bytes on little-endian systems and causes out of order data to be
	 * sent to the display).
	 */
	mipi->swap_bytes = true;

	ret = st7586_init(&spi->dev, mipi, &st7586_pipe_funcs, &st7586_driver,
			  &st7586_mode, rotation);
	if (ret)
		return ret;

	tdev = &mipi->tinydrm;

	ret = devm_tinydrm_register(tdev);
	if (ret)
		return ret;

	spi_set_drvdata(spi, mipi);

	DRM_DEBUG_DRIVER("Initialized %s:%s @%uMHz on minor %d\n",
			 tdev->drm->driver->name, dev_name(dev),
			 spi->max_speed_hz / 1000000,
			 tdev->drm->primary->index);

	return 0;
}

static void st7586_shutdown(struct spi_device *spi)
{
	struct mipi_dbi *mipi = spi_get_drvdata(spi);

	tinydrm_shutdown(&mipi->tinydrm);
}

static int __maybe_unused st7586_pm_suspend(struct device *dev)
{
	struct mipi_dbi *mipi = dev_get_drvdata(dev);

	return tinydrm_suspend(&mipi->tinydrm);
}

static int __maybe_unused st7586_pm_resume(struct device *dev)
{
	struct mipi_dbi *mipi = dev_get_drvdata(dev);

	return tinydrm_resume(&mipi->tinydrm);
}

static const struct dev_pm_ops st7586_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(st7586_pm_suspend, st7586_pm_resume)
};

static struct spi_driver st7586_spi_driver = {
	.driver = {
		.name = "st7586",
		.owner = THIS_MODULE,
		.of_match_table = st7586_of_match,
		.pm = &st7586_pm_ops,
	},
	.id_table = st7586_id,
	.probe = st7586_probe,
	.shutdown = st7586_shutdown,
};
module_spi_driver(st7586_spi_driver);

MODULE_DESCRIPTION("Sitronix ST7586 DRM driver");
MODULE_AUTHOR("David Lechner <david@lechnology.com>");
MODULE_LICENSE("GPL");
