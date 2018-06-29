// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Texas Instruments Incorporated -  http://www.ti.com/
 * Author: Benoit Parrot, <bparrot@ti.com>
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_plane_helper.h>

#include "omap_dmm_tiler.h"
#include "omap_drv.h"

/*
 * overlay funcs
 */
static void __maybe_unused
omap_overlay_atomic_print_state(struct drm_printer *p,
				const struct omap_global_state *state,
				struct omap_drm_private *priv)
{
	int i;

	drm_printf(p, "\tomap_global_state=%p\n", state);
	if (state) {
		for (i = 0; i < priv->num_ovls; i++) {
			struct drm_plane *plane =
				state->overlay.hwoverlay_to_plane[i];

			drm_printf(p, "\t\t[%d] plane=%p\n", i, plane);
			if (plane)
				drm_printf(p, "\t\t\t plane=%s\n", plane->name);
		}
	}
}

/* Global/shared object state funcs */

/*
 * This is a helper that returns the private state currently in operation.
 * Note that this would return the "old_state" if called in the atomic check
 * path, and the "new_state" after the atomic swap has been done.
 */
static struct omap_global_state *
omap_get_existing_global_state(struct omap_drm_private *priv)
{
	return to_omap_global_state(priv->glob_state.state);
}

/*
 * This acquires the modeset lock set aside for global state, creates
 * a new duplicated private object state.
 */
static struct omap_global_state *__must_check
omap_get_global_state(struct drm_atomic_state *s)
{
	struct omap_drm_private *priv = s->dev->dev_private;
	struct drm_private_state *priv_state;
	int ret;

	while (1) {
		ret = drm_modeset_lock(&priv->glob_state_lock, s->acquire_ctx);
		if (ret != -EDEADLK)
			break;

		drm_modeset_backoff(s->acquire_ctx);
	}

	if (ret)
		return ERR_PTR(ret);

	priv_state = drm_atomic_get_private_obj_state(s, &priv->glob_state);
	if (IS_ERR(priv_state))
		return ERR_CAST(priv_state);

	return to_omap_global_state(priv_state);
}

static struct drm_private_state *
omap_global_duplicate_state(struct drm_private_obj *obj)
{
	struct omap_global_state *state;

	state = kmemdup(obj->state, sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;

	__drm_atomic_helper_private_obj_duplicate_state(obj, &state->base);

	return &state->base;
}

static void omap_global_destroy_state(struct drm_private_obj *obj,
				      struct drm_private_state *state)
{
	struct omap_global_state *omap_state = to_omap_global_state(state);

	kfree(omap_state);
}

static const struct drm_private_state_funcs omap_global_state_funcs = {
	.atomic_duplicate_state = omap_global_duplicate_state,
	.atomic_destroy_state = omap_global_destroy_state,
};

int omap_global_obj_init(struct omap_drm_private *priv)
{
	struct omap_global_state *state;

	drm_modeset_lock_init(&priv->glob_state_lock);

	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	drm_atomic_private_obj_init(&priv->glob_state,
				    &state->base,
				    &omap_global_state_funcs);
	return 0;
}

void omap_global_obj_fini(struct omap_drm_private *priv)
{
	drm_atomic_private_obj_fini(&priv->glob_state);
	drm_modeset_lock_fini(&priv->glob_state_lock);
}

static struct omap_hw_overlay *
omap_plane_find_free_overlay(struct drm_device *dev,
			     struct omap_hw_overlay_state *new_state,
			     u32 caps, u32 fourcc, u32 crtc_mask)
{
	struct omap_drm_private *priv = dev->dev_private;
	const struct dispc_ops *ops = priv->dispc_ops;
	int i;

	DBG("caps: %x fourcc: %x crtc: %x\n", caps, fourcc, crtc_mask);

	for (i = 0; i < priv->num_ovls; i++) {
		struct omap_hw_overlay *cur = priv->overlays[i];
	
		DBG("%d: id: %d cur->caps: %x cur->crtc: %x\n",
		    cur->idx, cur->overlay_id, cur->caps, cur->possible_crtcs);

		/* skip if already in-use */
		if (new_state->hwoverlay_to_plane[cur->idx])
			continue;

		/* check if allowed on crtc */
		if (!(cur->possible_crtcs & crtc_mask))
			continue;

		/* skip if doesn't support some required caps: */
		if (caps & ~cur->caps)
			continue;

		/* check supported format */
		if (!ops->ovl_color_mode_supported(priv->dispc,
						   cur->overlay_id,
						   fourcc))
			continue;

		return cur;
	}

	DBG("no match\n");
	return NULL;
}

int omap_overlay_assign(struct drm_atomic_state *s, struct drm_plane *plane,
			u32 caps, u32 fourcc, u32 crtc_mask,
			struct omap_hw_overlay **overlay,
			struct omap_hw_overlay **r_overlay)
{
	struct omap_drm_private *priv = s->dev->dev_private;
	struct omap_global_state *new_global_state, *old_global_state;
	struct omap_hw_overlay_state *old_state, *new_state;
	struct omap_hw_overlay *ovl, *r_ovl;

	new_global_state = omap_get_global_state(s);
	if (IS_ERR(new_global_state))
		return PTR_ERR(new_global_state);

	/*
	 * grab old_state after omap_get_global_state(),
	 * since now we hold lock:
	 */
	old_global_state = omap_get_existing_global_state(priv);
	DBG("new_global_state: %p old_global_state: %p should be different (%d)",
	    new_global_state, old_global_state, new_global_state != old_global_state);

	old_state = &old_global_state->overlay;
	new_state = &new_global_state->overlay;

	if (!*overlay) {
		ovl = omap_plane_find_free_overlay(s->dev, new_state,
						   caps, fourcc, crtc_mask);
		if (!ovl)
			return -ENOMEM;

		new_state->hwoverlay_to_plane[ovl->idx] = plane;
		*overlay = ovl;

		if (r_overlay) {
			r_ovl = omap_plane_find_free_overlay(s->dev, new_state,
							     caps, fourcc,
							     crtc_mask);
			if (!r_ovl) {
				new_state->hwoverlay_to_plane[ovl->idx] = NULL;
				*overlay = NULL;
				return -ENOMEM;
			}

			new_state->hwoverlay_to_plane[r_ovl->idx] = plane;
			*r_overlay = r_ovl;
		}


		DBG("%s: assign to plane %s for caps %x",
		    (*overlay)->name, plane->name, caps);

		if (r_overlay) {
			DBG("%s: assign to right of plane %s for caps %x",
			    (*r_overlay)->name, plane->name, caps);
		}
	}

	return 0;
}

void omap_overlay_release(struct drm_atomic_state *s,
			  struct omap_hw_overlay *overlay)
{
	struct omap_global_state *state = omap_get_global_state(s);
	struct omap_hw_overlay_state *new_state = &state->overlay;

	if (!overlay)
		return;

	if (WARN_ON(!new_state->hwoverlay_to_plane[overlay->idx]))
		return;

	DBG("%s: release from plane %s", overlay->name,
	    new_state->hwoverlay_to_plane[overlay->idx]->name);

	new_state->hwoverlay_to_plane[overlay->idx] = NULL;
}

/*
 * This is called only from omap_atomic_commit_tail()
 * as a cleanup step to make sure hw overlay which are no longer
 * are disabled.
 *
 * I was originally taking the glob_state_lock here by calling
 * omap_get_global_state(s) but doing so here was causing all kind
 * lock related warnings, for instance:
 *  WARNING: CPU: 0 PID: 68 at drivers/gpu/drm/drm_modeset_lock.c:241
 * and
 *  WARNING: CPU: 0 PID: 68 at drivers/gpu/drm/drm_modeset_lock.c:244
 * As well as also generating these:
 *  ==================================
 *  WARNING: Nested lock was not taken
 *  4.18.0-rc2-00055-g5d51e5159b0a #24 Tainted: G        W
 *  ----------------------------------
 *  kworker/u2:3/66 is trying to lock:
 *  51abea2e (crtc_ww_class_mutex){+.+.}, at: drm_modeset_lock+0xd0/0x140
 *
 *  but this task is not holding:
 *  �21
 *
 * The only thing that worked so far was to stop trying to take that lock
 * in this particular case. It might the real solution but I would like
 * to sure.
 */
void omap_overlay_disable_unassigned(struct drm_atomic_state *s)
{
	struct omap_drm_private *priv = s->dev->dev_private;
	struct omap_hw_overlay_state *new_state;
	struct omap_global_state *old_state;
	int i;

	old_state = omap_get_existing_global_state(priv);
	new_state = &old_state->overlay;

	for (i = 0; i < priv->num_ovls; i++) {
		struct omap_hw_overlay *cur = priv->overlays[i];

		if (!new_state->hwoverlay_to_plane[cur->idx]) {
			priv->dispc_ops->ovl_enable(priv->dispc,
						    cur->overlay_id,
						    false);

			/*
			 * Since we are disabling this overlay in this
			 * atomic cycle we can reset the avalaible crtcs
			 * it can be used on
			 */
			cur->possible_crtcs = (1 << priv->num_crtcs) - 1;
		}
	}
}

void omap_overlay_destroy(struct omap_hw_overlay *overlay)
{
	kfree(overlay);
}

static struct omap_hw_overlay *omap_overlay_init(enum omap_plane_id overlay_id,
						 enum omap_overlay_caps caps)
{
	struct omap_hw_overlay *overlay;

	overlay = kzalloc(sizeof(*overlay), GFP_KERNEL);
	if (!overlay)
		return ERR_PTR(-ENOMEM);

	overlay->name = overlay2name(overlay_id);
	overlay->overlay_id = overlay_id;
	overlay->caps = caps;
	/* 
	 * When this is called priv->num_crtcs is not known yet.
	 * Use a safe mask value to start with, it will get updated to the
	 * proper value after the first use.
	 */
	overlay->possible_crtcs = 0xff;

	return overlay;
}

int omap_hwoverlays_init(struct omap_drm_private *priv)
{
	static const enum omap_plane_id overlays[] = {
			OMAP_DSS_GFX, OMAP_DSS_VIDEO1,
			OMAP_DSS_VIDEO2, OMAP_DSS_VIDEO3,
	};
	u32 num_overlays = priv->dispc_ops->get_num_ovls(priv->dispc);
	enum omap_overlay_caps caps;
	int i, ret;

	for (i = 0; i < num_overlays; i++) {
		struct omap_hw_overlay *overlay;

		caps = priv->dispc_ops->ovl_get_caps(priv->dispc, overlays[i]);
		overlay = omap_overlay_init(overlays[i], caps);
		if (IS_ERR(overlay)) {
			ret = PTR_ERR(overlay);
			dev_err(priv->dev, "failed to construct overlay for %s (%d)\n",
				overlay2name(i), ret);
			return ret;
		}
		overlay->idx = priv->num_ovls;
		priv->overlays[priv->num_ovls++] = overlay;
	}

	return 0;
}

void omap_hwoverlays_destroy(struct omap_drm_private *priv)
{
	int i;

	for (i = 0; i < priv->num_ovls; i++) {
		omap_overlay_destroy(priv->overlays[i]);
		priv->overlays[i] = NULL;
	}
}
