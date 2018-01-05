/*
 * Xilinx DRM KMS Framebuffer helper header
 *
 *  Copyright (C) 2015 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _XLNX_FB_H_
#define _XLNX_FB_H_

struct drm_fb_helper;

dma_addr_t
xlnx_fb_get_paddr(struct drm_framebuffer *base_fb, unsigned int plane);

void xlnx_fb_restore_mode(struct drm_fb_helper *fb_helper);
struct drm_framebuffer *
xlnx_fb_create(struct drm_device *drm, struct drm_file *file_priv,
	       const struct drm_mode_fb_cmd2 *mode_cmd);
void xlnx_fb_hotplug_event(struct drm_fb_helper *fb_helper);
struct drm_fb_helper *
xlnx_fb_init(struct drm_device *drm, int preferred_bpp,
	     unsigned int max_conn_count, unsigned int align,
	     unsigned int vres_mult);
void xlnx_fb_fini(struct drm_fb_helper *fb_helper);

#endif /* _XLNX_FB_H_ */
