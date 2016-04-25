/*
 * (C) COPYRIGHT 2016 ARM Limited. All rights reserved.
 * Author: Liviu Dudau <Liviu.Dudau@arm.com>
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * ARM Mali DP plane manipulation routines.
 */

#include <drm/drmP.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_plane_helper.h>

#include "malidp_hw.h"
#include "malidp_drv.h"

/* Layer specific register offsets */
#define MALIDP_LAYER_FORMAT		0x000
#define MALIDP_LAYER_CONTROL		0x004
#define   LAYER_ENABLE			(1 << 0)
#define   LAYER_ROT_OFFSET		8
#define   LAYER_H_FLIP			(1 << 10)
#define   LAYER_V_FLIP			(1 << 11)
#define   LAYER_ROT_MASK		(0xf << 8)
#define MALIDP_LAYER_SIZE		0x00c
#define   LAYER_H_VAL(x)		(((x) & 0x1fff) << 0)
#define   LAYER_V_VAL(x)		(((x) & 0x1fff) << 16)
#define MALIDP_LAYER_COMP_SIZE		0x010
#define MALIDP_LAYER_OFFSET		0x014
#define MALIDP_LAYER_STRIDE		0x018

static void malidp_de_plane_destroy(struct drm_plane *plane)
{
	struct malidp_plane *mp = to_malidp_plane(plane);

	if (mp->base.fb)
		drm_framebuffer_unreference(mp->base.fb);

	drm_plane_helper_disable(plane);
	drm_plane_cleanup(plane);
	devm_kfree(plane->dev->dev, mp);
}

static int malidp_de_atomic_update_plane(struct drm_plane *plane,
					 struct drm_crtc *crtc,
					 struct drm_framebuffer *fb,
					 int crtc_x, int crtc_y,
					 unsigned int crtc_w,
					 unsigned int crtc_h,
					 uint32_t src_x, uint32_t src_y,
					 uint32_t src_w, uint32_t src_h)
{
	return drm_atomic_helper_update_plane(plane, crtc, fb, crtc_x, crtc_y,
					      crtc_w, crtc_h, src_x, src_y,
					      src_w, src_h);
}

static int malidp_de_plane_atomic_set_property(struct drm_plane *plane,
					       struct drm_plane_state *state,
					       struct drm_property *property,
					       uint64_t val)
{
	return drm_atomic_helper_plane_set_property(plane, property, val);
}

static const struct drm_plane_funcs malidp_de_plane_funcs = {
	.update_plane = malidp_de_atomic_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.destroy = malidp_de_plane_destroy,
	.reset = drm_atomic_helper_plane_reset,
	.set_property = drm_atomic_helper_plane_set_property,
	.atomic_set_property = malidp_de_plane_atomic_set_property,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
};

static int malidp_de_plane_check(struct drm_plane *plane,
				 struct drm_plane_state *state)
{
	struct malidp_plane *mp = to_malidp_plane(plane);
	u32 src_w, src_h;

	if (!state->crtc || !state->fb)
		return 0;

	src_w = state->src_w >> 16;
	src_h = state->src_h >> 16;

	if ((state->crtc_w > mp->hwdev->max_line_size) ||
	    (state->crtc_h > mp->hwdev->max_line_size) ||
	    (state->crtc_w < mp->hwdev->min_line_size) ||
	    (state->crtc_h < mp->hwdev->min_line_size) ||
	    (state->crtc_w != src_w) || (state->crtc_h != src_h))
		return -EINVAL;

	/* packed RGB888 / BGR888 can't be rotated or flipped */
	if (state->rotation != BIT(DRM_ROTATE_0) &&
	    (state->fb->pixel_format == DRM_FORMAT_RGB888 ||
	     state->fb->pixel_format == DRM_FORMAT_BGR888))
		return -EINVAL;

	mp->rotmem_size = 0;
	if (state->rotation & MALIDP_ROTATED_MASK) {
		int val;

		val = mp->hwdev->rotmem_required(mp->hwdev, state->crtc_h,
						 state->crtc_w,
						 state->fb->pixel_format);
		if (val < 0)
			return val;

		mp->rotmem_size = val;
	}

	return 0;
}

static void malidp_de_plane_update(struct drm_plane *plane,
				   struct drm_plane_state *old_state)
{
	struct drm_gem_cma_object *obj;
	struct malidp_plane *mp;
	const struct malidp_hw_regmap *map;
	u8 format_id;
	u16 ptr;
	u32 format, src_w, src_h, dest_w, dest_h, val = 0;
	int num_planes, i;

	mp = to_malidp_plane(plane);

#ifdef MALIDP_ENABLE_BGND_COLOR_AS_PRIMARY_PLANE
	/* skip the primary plane, it is using the background color */
	if (!mp->layer || !mp->layer->id)
		return;
#endif

	map = &mp->hwdev->map;
	format = plane->state->fb->pixel_format;
	format_id = malidp_hw_get_format_id(map, mp->layer->id, format);
	if (format_id == (u8)-1)
		return;

	num_planes = drm_format_num_planes(format);

	/* convert src values from Q16 fixed point to integer */
	src_w = plane->state->src_w >> 16;
	src_h = plane->state->src_h >> 16;
	if (plane->state->rotation & MALIDP_ROTATED_MASK) {
		dest_w = plane->state->crtc_h;
		dest_h = plane->state->crtc_w;
	} else {
		dest_w = plane->state->crtc_w;
		dest_h = plane->state->crtc_h;
	}

	malidp_hw_write(mp->hwdev, format_id, mp->layer->base);

	for (i = 0; i < num_planes; i++) {
		/* calculate the offset for the layer's plane registers */
		ptr = mp->layer->ptr + (i << 4);

		obj = drm_fb_cma_get_gem_obj(plane->state->fb, i);
		malidp_hw_write(mp->hwdev, lower_32_bits(obj->paddr), ptr);
		malidp_hw_write(mp->hwdev, upper_32_bits(obj->paddr), ptr + 4);
		malidp_hw_write(mp->hwdev, plane->state->fb->pitches[i],
				mp->layer->base + MALIDP_LAYER_STRIDE);
	}

	malidp_hw_write(mp->hwdev, LAYER_H_VAL(src_w) | LAYER_V_VAL(src_h),
			mp->layer->base + MALIDP_LAYER_SIZE);

	malidp_hw_write(mp->hwdev, LAYER_H_VAL(dest_w) | LAYER_V_VAL(dest_h),
			mp->layer->base + MALIDP_LAYER_COMP_SIZE);

	malidp_hw_write(mp->hwdev, LAYER_H_VAL(plane->state->crtc_x) |
			LAYER_V_VAL(plane->state->crtc_y),
			mp->layer->base + MALIDP_LAYER_OFFSET);

	/* first clear the rotation bits in the register */
	malidp_hw_clearbits(mp->hwdev, LAYER_ROT_MASK,
			    mp->layer->base + MALIDP_LAYER_CONTROL);

	/* setup the rotation and axis flip bits */
	if (plane->state->rotation & DRM_ROTATE_MASK)
		val = ilog2(plane->state->rotation & DRM_ROTATE_MASK) << LAYER_ROT_OFFSET;
	if (plane->state->rotation & BIT(DRM_REFLECT_X))
		val |= LAYER_V_FLIP;
	if (plane->state->rotation & BIT(DRM_REFLECT_Y))
		val |= LAYER_H_FLIP;

	/* set the 'enable layer' bit */
	val |= LAYER_ENABLE;

	malidp_hw_setbits(mp->hwdev, val,
			  mp->layer->base + MALIDP_LAYER_CONTROL);
}

static void malidp_de_plane_disable(struct drm_plane *plane,
				    struct drm_plane_state *state)
{
	struct malidp_plane *mp = to_malidp_plane(plane);

	malidp_hw_clearbits(mp->hwdev, LAYER_ENABLE,
			    mp->layer->base + MALIDP_LAYER_CONTROL);
}

static const struct drm_plane_helper_funcs malidp_de_plane_helper_funcs = {
	.atomic_check = malidp_de_plane_check,
	.atomic_update = malidp_de_plane_update,
	.atomic_disable = malidp_de_plane_disable,
};

#ifdef MALIDP_ENABLE_BGND_COLOR_AS_PRIMARY_PLANE
static const uint32_t safe_modeset_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static int malidp_de_create_primary_plane(struct drm_device *drm)
{
	struct malidp_drm *malidp = drm->dev_private;
	struct malidp_plane *plane;
	int ret;

	plane = devm_kzalloc(drm->dev, sizeof(*plane), GFP_KERNEL);
	if (!plane)
		return -ENOMEM;

	ret = drm_universal_plane_init(drm, &plane->base, 0,
				       &malidp_de_plane_funcs,
				       safe_modeset_formats,
				       ARRAY_SIZE(safe_modeset_formats),
				       DRM_PLANE_TYPE_PRIMARY, NULL);
	if (ret)
		return ret;

	drm_plane_helper_add(&plane->base, &malidp_de_plane_helper_funcs);
	plane->hwdev = malidp->dev;

	return 0;
}
#endif

int malidp_de_planes_init(struct drm_device *drm)
{
	struct malidp_drm *malidp = drm->dev_private;
	const struct malidp_hw_regmap *map = &malidp->dev->map;
	struct malidp_plane *plane = NULL;
	enum drm_plane_type plane_type;
	unsigned long crtcs = 1 << drm->mode_config.num_crtc;
	u32 *formats;
	int ret, i, j, n;

#ifdef MALIDP_ENABLE_BGND_COLOR_AS_PRIMARY_PLANE
	ret = malidp_de_create_primary_plane(drm);
	if (ret)
		return ret;
	plane_type = DRM_PLANE_TYPE_OVERLAY;
#endif

	formats = kcalloc(map->n_input_formats, sizeof(*formats), GFP_KERNEL);
	if (!formats) {
		ret = -ENOMEM;
		goto cleanup;
	}

	for (i = 0; i < map->n_layers; i++) {
		u8 id = map->layers[i].id;

		plane = devm_kzalloc(drm->dev, sizeof(*plane), GFP_KERNEL);
		if (!plane) {
			ret = -ENOMEM;
			goto cleanup;
		}

		/* build the list of DRM supported formats based on the map */
		for (n = 0, j = 0;  j < map->n_input_formats; j++) {
			if ((map->input_formats[j].layer & id) == id)
				formats[n++] = map->input_formats[j].format;
		}

#ifndef MALIDP_ENABLE_BGND_COLOR_AS_PRIMARY_PLANE
		plane_type = (i == 0) ? DRM_PLANE_TYPE_PRIMARY :
					DRM_PLANE_TYPE_OVERLAY;
#endif
		ret = drm_universal_plane_init(drm, &plane->base, crtcs,
					       &malidp_de_plane_funcs, formats,
					       n, plane_type, NULL);
		if (ret < 0)
			goto cleanup;

		if (!drm->mode_config.rotation_property) {
			unsigned long flags = BIT(DRM_ROTATE_0) |
					      BIT(DRM_ROTATE_90) |
					      BIT(DRM_ROTATE_180) |
					      BIT(DRM_ROTATE_270) |
					      BIT(DRM_REFLECT_X) |
					      BIT(DRM_REFLECT_Y);
			drm->mode_config.rotation_property =
				drm_mode_create_rotation_property(drm, flags);
		}
		/* SMART layer can't be rotated */
		if (drm->mode_config.rotation_property && (id != DE_SMART))
			drm_object_attach_property(&plane->base.base,
						   drm->mode_config.rotation_property,
						   BIT(DRM_ROTATE_0));

		drm_plane_helper_add(&plane->base,
				     &malidp_de_plane_helper_funcs);
		plane->hwdev = malidp->dev;
		plane->layer = &map->layers[i];
	}

	kfree(formats);

	return 0;

cleanup:
	malidp_de_planes_destroy(drm);
	kfree(formats);

	return ret;
}

void malidp_de_planes_destroy(struct drm_device *drm)
{
	struct drm_plane *p, *pt;

	list_for_each_entry_safe(p, pt, &drm->mode_config.plane_list, head) {
		drm_plane_cleanup(p);
	}
}
