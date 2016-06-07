/*
 * Copyright(c) 2011-2016 Intel Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef _INTEL_GVT_H_
#define _INTEL_GVT_H_

#include "gvt/gvt.h"

/*
 * Under GVT-g, i915 host driver only owned limited graphics resources,
 * others are managed by GVT-g resource allocator and kept for other vGPUs.
 *
 * For graphics memory space partition, a typical layout looks like:
 *
 * +-------+-----------------------+------+-----------------------+
 * |* Host |   *GVT-g Resource     |* Host|   *GVT-g Resource     |
 * | Owned |   Allocator Managed   | Owned|   Allocator Managed   |
 * |       |                       |      |                       |
 * +---------------+-------+----------------------+-------+-------+
 * |       |       |       |       |      |       |       |       |
 * | i915  | vm 1  | vm 2  | vm 3  | i915 | vm 1  | vm 2  | vm 3  |
 * |       |       |       |       |      |       |       |       |
 * +-------+-------+-------+--------------+-------+-------+-------+
 * |           Aperture            |            Hidden            |
 * +-------------------------------+------------------------------+
 * |                       GGTT memory space                      |
 * +--------------------------------------------------------------+
 */

/* GGTT memory space owned by host */
/*
 * This amount is heavily related to the max screen resolution / multiple
 * display in *host*. If you are using a 4K monitor or multiple display
 * monitor, probably you should enlarge the low gm size.
 */
#define INTEL_GVT_HOST_LOW_GM_SIZE (96 * 1024 * 1024)

/*
 * This amount is related to the GPU workload in host. If you wish to run
 * heavy workload like 3D gaming, media transcoding *in host* and encounter
 * performance drops, probably you should enlarge the high gm size.
 */
#define INTEL_GVT_HOST_HIGH_GM_SIZE (384 * 1024 * 1024)

#ifdef CONFIG_DRM_I915_GVT
extern int intel_gvt_init(struct drm_i915_private *dev_priv);
extern void intel_gvt_cleanup(struct drm_i915_private *dev_priv);
extern int intel_gvt_init_device(struct drm_i915_private *dev_priv);
extern void intel_gvt_clean_device(struct drm_i915_private *dev_priv);
extern int intel_gvt_init_host(void);
#else
static inline int intel_gvt_init(struct drm_i915_private *dev_priv)
{
	return 0;
}
static inline void intel_gvt_cleanup(struct drm_i915_private *dev_priv)
{
}
#endif

#endif /* _INTEL_GVT_H_ */
