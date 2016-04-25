/*
 * (C) COPYRIGHT 2016 ARM Limited. All rights reserved.
 * Author: Liviu Dudau <Liviu.Dudau@arm.com>
 *
 * This program is free software and is provided to you under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation, and any use by you of this program is subject to the terms
 * of such GNU licence.
 *
 * ARM Mali DP500/DP550/DP650 KMS/DRM driver structures
 */

#ifndef __MALIDP_DRV_H__
#define __MALIDP_DRV_H__

#include <linux/mutex.h>
#include <linux/wait.h>
#include "malidp_hw.h"

struct malidp_drm {
	struct malidp_hw_device *dev;
	struct drm_fbdev_cma *fbdev;
	struct list_head event_list;
	struct drm_crtc crtc;
	wait_queue_head_t wq;
	struct {
		struct drm_atomic_state *state;
		struct work_struct work;
		struct mutex lock;
	} commit;
	atomic_t config_valid;
};

#define crtc_to_malidp_device(x) container_of(x, struct malidp_drm, crtc)

struct malidp_plane {
	struct drm_plane base;
	struct malidp_hw_device *hwdev;
	const struct malidp_layer *layer;
	/* size of the required rotation memory when plane is rotated */
	u32 rotmem_size;
};

#define to_malidp_plane(x) container_of(x, struct malidp_plane, base)

int malidp_set_and_wait_config_valid(struct drm_device *drm);
int malidp_de_planes_init(struct drm_device *drm);
void malidp_de_planes_destroy(struct drm_device *drm);
int malidp_crtc_init(struct drm_device *drm);

/* often used combination of rotational bits */
#define MALIDP_ROTATED_MASK	(BIT(DRM_ROTATE_90) | BIT(DRM_ROTATE_270))

#endif  /* __MALIDP_DRV_H__ */
