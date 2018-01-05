/*
 * Xilinx DRM KMS GEM helper header
 *
 *  Copyright (C) 2015 - 2018 Xilinx, Inc.
 *
 *  Author: Hyun Woo Kwon <hyun.kwon@xilinx.com>
 *
 * SPDX-License-Identifier: GPL-2.0
 */

#ifndef _XLNX_GEM_H_
#define _XLNX_GEM_H_

int xlnx_gem_cma_dumb_create(struct drm_file *file_priv,
			     struct drm_device *drm,
			     struct drm_mode_create_dumb *args);

#endif /* _XLNX_GEM_H_ */
