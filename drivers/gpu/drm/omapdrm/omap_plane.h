/*
 * omap_plane.h -- OMAP DRM Plane
 *
 * Copyright (C) 2011 Texas Instruments
 * Author: Rob Clark <rob@ti.com>
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

#ifndef __OMAPDRM_PLANE_H__
#define __OMAPDRM_PLANE_H__

#include <linux/types.h>

enum drm_plane_type;

struct drm_device;
struct drm_mode_object;
struct drm_plane;

/*
 * Atomic plane state. Subclasses the base drm_plane_state in order to
 * track assigned overlay and hw specific state.
 */
struct omap_plane_state {
	struct drm_plane_state base;

	struct omap_hw_overlay *overlay;
	struct omap_hw_overlay *r_overlay;  /* right overlay */
};
#define to_omap_plane_state(x) \
		container_of(x, struct omap_plane_state, base)

struct drm_plane *omap_plane_init(struct drm_device *dev,
		int idx, enum drm_plane_type type,
		u32 possible_crtcs);
void omap_plane_install_properties(struct drm_plane *plane,
		struct drm_mode_object *obj);

static inline bool is_omap_plane_dual_overlay(struct drm_plane_state *state)
{
	struct omap_plane_state *omap_state = to_omap_plane_state(state);

	return !!omap_state->r_overlay;
}

#endif /* __OMAPDRM_PLANE_H__ */
