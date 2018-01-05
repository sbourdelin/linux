/*
 * Xilinx DRM KMS Header for Xilinx
 *
 *  Copyright (C) 2013 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyunk@xilinx.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _XLNX_DRV_H_
#define _XLNX_DRV_H_

struct drm_device;
struct xlnx_crtc_helper;

uint32_t xlnx_get_format(struct drm_device *drm);
unsigned int xlnx_get_align(struct drm_device *drm);
struct xlnx_crtc_helper *xlnx_get_crtc_helper(struct drm_device *drm);
struct xlnx_bridge_helper *xlnx_get_bridge_helper(struct drm_device *drm);

#endif /* _XLNX_DRV_H_ */
