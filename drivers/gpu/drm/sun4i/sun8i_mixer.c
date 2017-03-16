/*
 * Copyright (C) 2017 Icenowy Zheng <icenowy@aosc.xyz>
 *
 * Based on sun4i_backend.c, which is:
 *   Copyright (C) 2015 Free Electrons
 *   Copyright (C) 2015 NextThing Co
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_plane_helper.h>

#include <linux/component.h>
#include <linux/reset.h>
#include <linux/of_device.h>

#include "sun8i_mixer.h"
#include "sun4i_drv.h"

void sun8i_mixer_commit(struct sun8i_mixer *mixer)
{
	DRM_DEBUG_DRIVER("Committing changes\n");

	regmap_write(mixer->regs, SUN8I_MIXER_GLOBAL_DBUFF,
		     SUN8I_MIXER_GLOBAL_DBUFF_ENABLE);
}
EXPORT_SYMBOL(sun8i_mixer_commit);

void sun8i_mixer_layer_enable(struct sun8i_mixer *mixer,
				int layer, bool enable)
{
	u32 val;
	/* Currently the first UI channel is used */
	int chan = mixer->cfg->vi_num;

	DRM_DEBUG_DRIVER("Enabling layer %d in channel %d\n", layer, chan);

	if (enable)
		val = SUN8I_MIXER_CHAN_UI_LAYER_ATTR_EN;
	else
		val = 0;

	regmap_update_bits(mixer->regs,
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR(chan, layer),
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR_EN, val);

	/* Set the alpha configuration */
	regmap_update_bits(mixer->regs,
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR(chan, layer),
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA_MODE_MASK,
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA_MODE_DEF);
	regmap_update_bits(mixer->regs,
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR(chan, layer),
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA_MASK,
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR_ALPHA_DEF);
}
EXPORT_SYMBOL(sun8i_mixer_layer_enable);

static int sun8i_mixer_drm_format_to_layer(struct drm_plane *plane,
					     u32 format, u32 *mode)
{
	if ((plane->type == DRM_PLANE_TYPE_PRIMARY) &&
	    (format == DRM_FORMAT_ARGB8888))
		format = DRM_FORMAT_XRGB8888;

	switch (format) {
	case DRM_FORMAT_ARGB8888:
		*mode = SUN8I_MIXER_CHAN_UI_LAYER_ATTR_FBFMT_ARGB8888;
		break;

	case DRM_FORMAT_XRGB8888:
		*mode = SUN8I_MIXER_CHAN_UI_LAYER_ATTR_FBFMT_XRGB8888;
		break;

	case DRM_FORMAT_RGB888:
		*mode = SUN8I_MIXER_CHAN_UI_LAYER_ATTR_FBFMT_RGB888;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

int sun8i_mixer_update_layer_coord(struct sun8i_mixer *mixer,
				     int layer, struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	/* Currently the first UI channel is used */
	int chan = mixer->cfg->vi_num;
	int i;

	DRM_DEBUG_DRIVER("Updating layer %d\n", layer);

	if (plane->type == DRM_PLANE_TYPE_PRIMARY) {
		DRM_DEBUG_DRIVER("Primary layer, updating global size W: %u H: %u\n",
				 state->crtc_w, state->crtc_h);
		regmap_write(mixer->regs, SUN8I_MIXER_GLOBAL_SIZE,
			     SUN8I_MIXER_SIZE(state->crtc_w,
					      state->crtc_h));
		DRM_DEBUG_DRIVER("Updating blender size\n");
		for (i = 0; i < SUN8I_MIXER_MAX_CHAN_COUNT; i++)
			regmap_write(mixer->regs,
				     SUN8I_MIXER_BLEND_ATTR_INSIZE(i),
				     SUN8I_MIXER_SIZE(state->crtc_w,
						      state->crtc_h));
		regmap_write(mixer->regs, SUN8I_MIXER_BLEND_OUTSIZE,
			     SUN8I_MIXER_SIZE(state->crtc_w,
					      state->crtc_h));
		DRM_DEBUG_DRIVER("Updating channel size\n");
		regmap_write(mixer->regs, SUN8I_MIXER_CHAN_UI_OVL_SIZE(chan),
			     SUN8I_MIXER_SIZE(state->crtc_w,
					      state->crtc_h));
	}

	/* Set the line width */
	DRM_DEBUG_DRIVER("Layer line width: %d bytes\n", fb->pitches[0]);
	regmap_write(mixer->regs, SUN8I_MIXER_CHAN_UI_LAYER_PITCH(chan, layer),
		     fb->pitches[0]);

	/* Set height and width */
	DRM_DEBUG_DRIVER("Layer size W: %u H: %u\n",
			 state->crtc_w, state->crtc_h);
	regmap_write(mixer->regs, SUN8I_MIXER_CHAN_UI_LAYER_SIZE(chan, layer),
		     SUN8I_MIXER_SIZE(state->crtc_w, state->crtc_h));

	/* Set base coordinates */
	DRM_DEBUG_DRIVER("Layer coordinates X: %d Y: %d\n",
			 state->crtc_x, state->crtc_y);
	regmap_write(mixer->regs, SUN8I_MIXER_CHAN_UI_LAYER_COORD(chan, layer),
		     SUN8I_MIXER_COORD(state->crtc_x, state->crtc_y));

	return 0;
}
EXPORT_SYMBOL(sun8i_mixer_update_layer_coord);

int sun8i_mixer_update_layer_formats(struct sun8i_mixer *mixer,
				       int layer, struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	bool interlaced = false;
	u32 val;
	/* Currently the first UI channel is used */
	int chan = mixer->cfg->vi_num;
	int ret;

	if (plane->state->crtc)
		interlaced = plane->state->crtc->state->adjusted_mode.flags
			& DRM_MODE_FLAG_INTERLACE;

	regmap_update_bits(mixer->regs, SUN8I_MIXER_BLEND_OUTCTL,
			   SUN8I_MIXER_BLEND_OUTCTL_INTERLACED,
			   interlaced ?
			   SUN8I_MIXER_BLEND_OUTCTL_INTERLACED : 0);

	DRM_DEBUG_DRIVER("Switching display mixer interlaced mode %s\n",
			 interlaced ? "on" : "off");

	ret = sun8i_mixer_drm_format_to_layer(plane, fb->format->format,
						&val);
	if (ret) {
		DRM_DEBUG_DRIVER("Invalid format\n");
		return ret;
	}

	regmap_update_bits(mixer->regs,
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR(chan, layer),
			   SUN8I_MIXER_CHAN_UI_LAYER_ATTR_FBFMT_MASK, val);

	return 0;
}
EXPORT_SYMBOL(sun8i_mixer_update_layer_formats);

int sun8i_mixer_update_layer_buffer(struct sun8i_mixer *mixer,
				      int layer, struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_cma_object *gem;
	dma_addr_t paddr;
	uint32_t paddr_u32;
	/* Currently the first UI channel is used */
	int chan = mixer->cfg->vi_num;
	int bpp;

	/* Get the physical address of the buffer in memory */
	gem = drm_fb_cma_get_gem_obj(fb, 0);

	DRM_DEBUG_DRIVER("Using GEM @ %pad\n", &gem->paddr);

	/* Compute the start of the displayed memory */
	bpp = fb->format->cpp[0];
	paddr = gem->paddr + fb->offsets[0];
	paddr += (state->src_x >> 16) * bpp;
	paddr += (state->src_y >> 16) * fb->pitches[0];

	DRM_DEBUG_DRIVER("Setting buffer address to %pad\n", &paddr);

	paddr_u32 = (uint32_t) paddr;

	regmap_write(mixer->regs,
		     SUN8I_MIXER_CHAN_UI_LAYER_TOP_LADDR(chan, layer),
		     paddr_u32);

	return 0;
}
EXPORT_SYMBOL(sun8i_mixer_update_layer_buffer);

static struct regmap_config sun8i_mixer_regmap_config = {
	.reg_bits	= 32,
	.val_bits	= 32,
	.reg_stride	= 4,
	.max_register	= 0xbffc, /* guessed */
};

static int sun8i_mixer_bind(struct device *dev, struct device *master,
			      void *data)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct drm_device *drm = data;
	struct sun4i_drv *drv = drm->dev_private;
	struct sun8i_mixer *mixer;
	struct resource *res;
	void __iomem *regs;
	int i, ret;

	mixer = devm_kzalloc(dev, sizeof(*mixer), GFP_KERNEL);
	if (!mixer)
		return -ENOMEM;
	dev_set_drvdata(dev, mixer);
	drv->mixer = mixer;

	mixer->cfg = of_device_get_match_data(dev);
	if (!mixer->cfg)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	mixer->regs = devm_regmap_init_mmio(dev, regs,
					      &sun8i_mixer_regmap_config);
	if (IS_ERR(mixer->regs)) {
		dev_err(dev, "Couldn't create the mixer regmap\n");
		return PTR_ERR(mixer->regs);
	}

	mixer->reset = devm_reset_control_get(dev, NULL);
	if (IS_ERR(mixer->reset)) {
		dev_err(dev, "Couldn't get our reset line\n");
		return PTR_ERR(mixer->reset);
	}

	ret = reset_control_deassert(mixer->reset);
	if (ret) {
		dev_err(dev, "Couldn't deassert our reset line\n");
		return ret;
	}

	mixer->bus_clk = devm_clk_get(dev, "bus");
	if (IS_ERR(mixer->bus_clk)) {
		dev_err(dev, "Couldn't get the mixer bus clock\n");
		ret = PTR_ERR(mixer->bus_clk);
		goto err_assert_reset;
	}
	clk_prepare_enable(mixer->bus_clk);

	mixer->mod_clk = devm_clk_get(dev, "mod");
	if (IS_ERR(mixer->mod_clk)) {
		dev_err(dev, "Couldn't get the mixer module clock\n");
		ret = PTR_ERR(mixer->mod_clk);
		goto err_disable_bus_clk;
	}
	clk_prepare_enable(mixer->mod_clk);

	/* Reset the registers */
	for (i = 0x0; i < 0x20000; i += 4)
		regmap_write(mixer->regs, i, 0);

	/* Enable the mixer */
	regmap_write(mixer->regs, SUN8I_MIXER_GLOBAL_CTL,
		     SUN8I_MIXER_GLOBAL_CTL_RT_EN);

	/* Initialize blender */
	regmap_write(mixer->regs, SUN8I_MIXER_BLEND_FCOLOR_CTL,
		     SUN8I_MIXER_BLEND_FCOLOR_CTL_DEF);
	regmap_write(mixer->regs, SUN8I_MIXER_BLEND_PREMULTIPLY,
		     SUN8I_MIXER_BLEND_PREMULTIPLY_DEF);
	regmap_write(mixer->regs, SUN8I_MIXER_BLEND_BKCOLOR,
		     SUN8I_MIXER_BLEND_BKCOLOR_DEF);
	regmap_write(mixer->regs, SUN8I_MIXER_BLEND_MODE(0),
		     SUN8I_MIXER_BLEND_MODE_DEF);
	regmap_write(mixer->regs, SUN8I_MIXER_BLEND_MODE(1),
		     SUN8I_MIXER_BLEND_MODE_DEF);
	regmap_write(mixer->regs, SUN8I_MIXER_BLEND_CK_CTL,
		     SUN8I_MIXER_BLEND_CK_CTL_DEF);

	for (i = 0; i < SUN8I_MIXER_MAX_CHAN_COUNT; i++)
		regmap_write(mixer->regs,
			     SUN8I_MIXER_BLEND_ATTR_FCOLOR(i),
			     SUN8I_MIXER_BLEND_ATTR_FCOLOR_DEF);

	/* Select the first UI channel */
	DRM_DEBUG_DRIVER("Selecting channel %d (first UI channel)\n",
			 mixer->cfg->vi_num);
	regmap_write(mixer->regs, SUN8I_MIXER_BLEND_ROUTE,
		     mixer->cfg->vi_num);

	return 0;

	clk_disable_unprepare(mixer->mod_clk);
err_disable_bus_clk:
	clk_disable_unprepare(mixer->bus_clk);
err_assert_reset:
	reset_control_assert(mixer->reset);
	return ret;
}

static void sun8i_mixer_unbind(struct device *dev, struct device *master,
				 void *data)
{
	struct sun8i_mixer *mixer = dev_get_drvdata(dev);

	clk_disable_unprepare(mixer->mod_clk);
	clk_disable_unprepare(mixer->bus_clk);
	reset_control_assert(mixer->reset);
}

static const struct component_ops sun8i_mixer_ops = {
	.bind	= sun8i_mixer_bind,
	.unbind	= sun8i_mixer_unbind,
};

static int sun8i_mixer_probe(struct platform_device *pdev)
{
	return component_add(&pdev->dev, &sun8i_mixer_ops);
}

static int sun8i_mixer_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &sun8i_mixer_ops);

	return 0;
}

static const struct sun8i_mixer_cfg sun8i_v3s_mixer_cfg = {
	.vi_num = 2,
	.ui_num = 1,
};

static const struct of_device_id sun8i_mixer_of_table[] = {
	{
		.compatible = "allwinner,sun8i-v3s-de2-mixer",
		.data = &sun8i_v3s_mixer_cfg
	},
	{ }
};
MODULE_DEVICE_TABLE(of, sun8i_mixer_of_table);

static struct platform_driver sun8i_mixer_platform_driver = {
	.probe		= sun8i_mixer_probe,
	.remove		= sun8i_mixer_remove,
	.driver		= {
		.name		= "sun8i-mixer",
		.of_match_table	= sun8i_mixer_of_table,
	},
};
module_platform_driver(sun8i_mixer_platform_driver);

MODULE_AUTHOR("Icenowy Zheng <icenowy@aosc.xyz>");
MODULE_DESCRIPTION("Allwinner DE2 Mixer driver");
MODULE_LICENSE("GPL");
