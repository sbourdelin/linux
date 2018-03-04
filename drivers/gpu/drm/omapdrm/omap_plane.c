/*
 * Copyright (C) 2011 Texas Instruments Incorporated - http://www.ti.com/
 * Author: Rob Clark <rob.clark@linaro.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_plane_helper.h>

#include "omap_dmm_tiler.h"
#include "omap_drv.h"

/*
 * plane funcs
 */

#define to_omap_plane(x) container_of(x, struct omap_plane, base)

struct omap_plane {
	struct drm_plane base;
	enum omap_plane_id main_id;
	enum omap_plane_id aux_id;
	const char *name;
	bool virtual_plane;
};

static const char *plane_id_to_name[];

static int omap_plane_prepare_fb(struct drm_plane *plane,
				 struct drm_plane_state *new_state)
{
	if (!new_state->fb)
		return 0;

	return omap_framebuffer_pin(new_state->fb);
}

static void omap_plane_cleanup_fb(struct drm_plane *plane,
				  struct drm_plane_state *old_state)
{
	if (old_state->fb)
		omap_framebuffer_unpin(old_state->fb);
}

static void omap_plane_atomic_update(struct drm_plane *plane,
				     struct drm_plane_state *old_state)
{
	struct omap_drm_private *priv = plane->dev->dev_private;
	struct omap_plane *omap_plane = to_omap_plane(plane);
	struct drm_plane_state *state = plane->state;
	struct omap_overlay_info main_info, aux_info;
	int ret;
	bool dual_plane = omap_plane->virtual_plane;

	DBG("%s, crtc=%p fb=%p", omap_plane->name, state->crtc, state->fb);

	memset(&main_info, 0, sizeof(main_info));
	main_info.rotation_type = OMAP_DSS_ROT_NONE;
	main_info.rotation = DRM_MODE_ROTATE_0;
	main_info.global_alpha = 0xff;
	main_info.zorder = state->zpos;

	aux_info = main_info;

	/* update scanout: */
	omap_framebuffer_update_scanout(state->fb, state, &main_info,
					dual_plane ? &aux_info : NULL);

	DBG("%s: %dx%d -> %dx%d (%d)",
	    plane_id_to_name[omap_plane->main_id],
	    main_info.width, main_info.height,
	    main_info.out_width, main_info.out_height,
	    main_info.screen_width);
	DBG("%d,%d %pad %pad", main_info.pos_x, main_info.pos_y,
	    &main_info.paddr, &main_info.p_uv_addr);

	if (dual_plane) {
		aux_info.zorder = main_info.zorder + 1; // XXX

		DBG("%s: %dx%d -> %dx%d (%d)",
		    plane_id_to_name[omap_plane->aux_id],
		    aux_info.width, aux_info.height,
		    aux_info.out_width, aux_info.out_height,
		    aux_info.screen_width);
		DBG("%d,%d %pad %pad", aux_info.pos_x, aux_info.pos_y,
		    &aux_info.paddr, &aux_info.p_uv_addr);
	}

	ret = priv->dispc_ops->ovl_setup(omap_plane->main_id, &main_info,
			      omap_crtc_timings(state->crtc), false,
			      omap_crtc_channel(state->crtc));
	if (ret) {
		dev_err(plane->dev->dev, "Failed to setup plane1 %s\n",
			omap_plane->name);
		priv->dispc_ops->ovl_enable(omap_plane->main_id, false);
		return;
	}

	priv->dispc_ops->ovl_enable(omap_plane->main_id, true);

	if (dual_plane) {
		ret = priv->dispc_ops->ovl_setup(omap_plane->aux_id, &aux_info,
				      omap_crtc_timings(state->crtc), false,
				      omap_crtc_channel(state->crtc));
		if (ret) {
			dev_err(plane->dev->dev, "Failed to setup plane2 %s\n",
				omap_plane->name);
			priv->dispc_ops->ovl_enable(omap_plane->aux_id, false);
			priv->dispc_ops->ovl_enable(omap_plane->main_id, false);
			return;
		}

		priv->dispc_ops->ovl_enable(omap_plane->aux_id, true);
	}
}

static void omap_plane_atomic_disable(struct drm_plane *plane,
				      struct drm_plane_state *old_state)
{
	struct omap_drm_private *priv = plane->dev->dev_private;
	struct omap_plane *omap_plane = to_omap_plane(plane);
	bool dual_plane = omap_plane->virtual_plane;

	plane->state->rotation = DRM_MODE_ROTATE_0;
	plane->state->zpos = plane->type == DRM_PLANE_TYPE_PRIMARY
			   ? 0 : omap_plane->main_id;

	priv->dispc_ops->ovl_enable(omap_plane->main_id, false);
	if (dual_plane)
		priv->dispc_ops->ovl_enable(omap_plane->aux_id, false);
}

static int omap_plane_atomic_check(struct drm_plane *plane,
				   struct drm_plane_state *state)
{
	struct drm_crtc_state *crtc_state;

	if (!state->fb)
		return 0;

	/* crtc should only be NULL when disabling (i.e., !state->fb) */
	if (WARN_ON(!state->crtc))
		return 0;

	crtc_state = drm_atomic_get_existing_crtc_state(state->state, state->crtc);
	/* we should have a crtc state if the plane is attached to a crtc */
	if (WARN_ON(!crtc_state))
		return 0;

	if (!crtc_state->enable)
		return 0;

	if (state->crtc_x < 0 || state->crtc_y < 0)
		return -EINVAL;

	if (state->crtc_x + state->crtc_w > crtc_state->adjusted_mode.hdisplay)
		return -EINVAL;

	if (state->crtc_y + state->crtc_h > crtc_state->adjusted_mode.vdisplay)
		return -EINVAL;

	if (state->rotation != DRM_MODE_ROTATE_0 &&
	    !omap_framebuffer_supports_rotation(state->fb))
		return -EINVAL;

	return 0;
}

static const struct drm_plane_helper_funcs omap_plane_helper_funcs = {
	.prepare_fb = omap_plane_prepare_fb,
	.cleanup_fb = omap_plane_cleanup_fb,
	.atomic_check = omap_plane_atomic_check,
	.atomic_update = omap_plane_atomic_update,
	.atomic_disable = omap_plane_atomic_disable,
};

static void omap_plane_destroy(struct drm_plane *plane)
{
	struct omap_plane *omap_plane = to_omap_plane(plane);

	DBG("%s", omap_plane->name);

	drm_plane_cleanup(plane);

	kfree(omap_plane);
}

/* helper to install properties which are common to planes and crtcs */
void omap_plane_install_properties(struct drm_plane *plane,
		struct drm_mode_object *obj)
{
	struct drm_device *dev = plane->dev;
	struct omap_drm_private *priv = dev->dev_private;

	if (priv->has_dmm) {
		if (!plane->rotation_property)
			drm_plane_create_rotation_property(plane,
							   DRM_MODE_ROTATE_0,
							   DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_90 |
							   DRM_MODE_ROTATE_180 | DRM_MODE_ROTATE_270 |
							   DRM_MODE_REFLECT_X | DRM_MODE_REFLECT_Y);

		/* Attach the rotation property also to the crtc object */
		if (plane->rotation_property && obj != &plane->base)
			drm_object_attach_property(obj, plane->rotation_property,
						   DRM_MODE_ROTATE_0);
	}

	drm_object_attach_property(obj, priv->zorder_prop, 0);
}

static void omap_plane_reset(struct drm_plane *plane)
{
	struct omap_plane *omap_plane = to_omap_plane(plane);

	drm_atomic_helper_plane_reset(plane);
	if (!plane->state)
		return;

	/*
	 * Set the zpos default depending on whether we are a primary or overlay
	 * plane.
	 */
	plane->state->zpos = plane->type == DRM_PLANE_TYPE_PRIMARY
			   ? 0 : omap_plane->main_id;
}

static int omap_plane_atomic_set_property(struct drm_plane *plane,
					  struct drm_plane_state *state,
					  struct drm_property *property,
					  uint64_t val)
{
	struct omap_drm_private *priv = plane->dev->dev_private;

	if (property == priv->zorder_prop)
		state->zpos = val;
	else
		return -EINVAL;

	return 0;
}

static int omap_plane_atomic_get_property(struct drm_plane *plane,
					  const struct drm_plane_state *state,
					  struct drm_property *property,
					  uint64_t *val)
{
	struct omap_drm_private *priv = plane->dev->dev_private;

	if (property == priv->zorder_prop)
		*val = state->zpos;
	else
		return -EINVAL;

	return 0;
}

static const struct drm_plane_funcs omap_plane_funcs = {
	.update_plane = drm_atomic_helper_update_plane,
	.disable_plane = drm_atomic_helper_disable_plane,
	.reset = omap_plane_reset,
	.destroy = omap_plane_destroy,
	.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_plane_destroy_state,
	.atomic_set_property = omap_plane_atomic_set_property,
	.atomic_get_property = omap_plane_atomic_get_property,
};

static const char *plane_id_to_name[] = {
	[OMAP_DSS_GFX] = "gfx",
	[OMAP_DSS_VIDEO1] = "vid1",
	[OMAP_DSS_VIDEO2] = "vid2",
	[OMAP_DSS_VIDEO3] = "vid3",
};

static const char * const virtual_plane_id_to_name[] = {
	[OMAP_DSS_GFX] = "virt-gfx",
	[OMAP_DSS_VIDEO1] = "virt-vid1",
	[OMAP_DSS_VIDEO2] = "virt-vid2",
	[OMAP_DSS_VIDEO3] = "virt-vid3",
};

struct drm_plane *omap_plane_init(struct drm_device *dev,
		int idx, enum drm_plane_type type,
		u32 possible_crtcs,
		struct dispc_plane_mappings *plane_mappings)
{
	struct omap_drm_private *priv = dev->dev_private;
	unsigned int num_planes = priv->dispc_ops->get_num_ovls();
	struct drm_plane *plane;
	struct omap_plane *omap_plane;
	int ret;
	u32 nformats;
	const u32 *formats;

	if (plane_mappings->num_planes)
		num_planes = plane_mappings->num_planes;

	if (WARN_ON(idx >= num_planes))
		return ERR_PTR(-EINVAL);

	omap_plane = kzalloc(sizeof(*omap_plane), GFP_KERNEL);
	if (!omap_plane)
		return ERR_PTR(-ENOMEM);

	if (plane_mappings->num_planes) {
		/* Use plane data from DT */
		omap_plane->main_id = plane_mappings->plane[idx].main_id;
		if (plane_mappings->plane[idx].is_virtual) {
			omap_plane->name =
				virtual_plane_id_to_name[omap_plane->main_id];
			omap_plane->aux_id = plane_mappings->plane[idx].aux_id;
			omap_plane->virtual_plane = true;
		} else {
			omap_plane->name =
				plane_id_to_name[omap_plane->main_id];
		}
		if (plane_mappings->plane[idx].crtc_mask)
			possible_crtcs = plane_mappings->plane[idx].crtc_mask;
	} else {
		/* Use legacy plane allocation */
		omap_plane->main_id = idx;
		omap_plane->name = plane_id_to_name[idx];
	}

	DBG("%s: type=%d", omap_plane->name, type);
	DBG("	omap_plane->main_id: %d", omap_plane->main_id);
	if (omap_plane->virtual_plane)
		DBG("	omap_plane->aux_id: %d", omap_plane->aux_id);
	DBG("	crtc_mask: 0x%04x", possible_crtcs);

	formats = priv->dispc_ops->ovl_get_color_modes(omap_plane->main_id);
	for (nformats = 0; formats[nformats]; ++nformats)
		;

	plane = &omap_plane->base;

	ret = drm_universal_plane_init(dev, plane, possible_crtcs,
				       &omap_plane_funcs, formats,
				       nformats, NULL, type, NULL);
	if (ret < 0)
		goto error;

	drm_plane_helper_add(plane, &omap_plane_helper_funcs);

	omap_plane_install_properties(plane, &plane->base);
	drm_plane_create_zpos_property(plane, 0, 0, num_planes - 1);

	return plane;

error:
	dev_err(dev->dev, "%s(): could not create plane: %s\n",
		__func__, omap_plane->name);

	kfree(omap_plane);
	return NULL;
}
