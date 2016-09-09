/*
 * Copyright © 2015 Intel Corporation
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
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */
#include <linux/firmware.h>
#include <asm/msr-index.h>
#include "i915_drv.h"
#include "intel_guc.h"

static unsigned int slpc_get_platform_sku(struct drm_i915_private *dev_priv)
{
	enum slpc_platform_sku platform_sku;

	if (IS_SKL_ULX(dev_priv))
		platform_sku = SLPC_PLATFORM_SKU_ULX;
	else if (IS_SKL_ULT(dev_priv))
		platform_sku = SLPC_PLATFORM_SKU_ULT;
	else
		platform_sku = SLPC_PLATFORM_SKU_DT;

	WARN_ON(platform_sku > 0xFF);

	return platform_sku;
}

static unsigned int slpc_get_slice_count(struct drm_i915_private *dev_priv)
{
	unsigned int slice_count = 1;

	if (IS_SKYLAKE(dev_priv))
		slice_count = hweight8(INTEL_INFO(dev_priv)->sseu.slice_mask);

	return slice_count;
}

static void slpc_shared_data_init(struct drm_i915_private *dev_priv)
{
	struct drm_i915_gem_object *obj;
	struct page *page;
	struct slpc_shared_data *data;
	u64 msr_value;

	if (!dev_priv->guc.slpc.vma)
		return;

	obj = dev_priv->guc.slpc.vma->obj;

	page = i915_gem_object_get_page(obj, 0);
	if (page) {
		data = kmap_atomic(page);
		memset(data, 0, sizeof(struct slpc_shared_data));

		data->shared_data_size = sizeof(struct slpc_shared_data);
		data->global_state = (u32)SLPC_GLOBAL_STATE_NOT_RUNNING;
		data->platform_info.platform_sku =
					(u8)slpc_get_platform_sku(dev_priv);
		data->platform_info.slice_count =
					(u8)slpc_get_slice_count(dev_priv);
		data->platform_info.power_plan_source =
			(u8)SLPC_POWER_PLAN_SOURCE(SLPC_POWER_PLAN_BALANCED,
						    SLPC_POWER_SOURCE_AC);
		rdmsrl(MSR_TURBO_RATIO_LIMIT, msr_value);
		data->platform_info.P0_freq = (u8)msr_value;
		rdmsrl(MSR_PLATFORM_INFO, msr_value);
		data->platform_info.P1_freq = (u8)(msr_value >> 8);
		data->platform_info.Pe_freq = (u8)(msr_value >> 40);
		data->platform_info.Pn_freq = (u8)(msr_value >> 48);

		kunmap_atomic(data);
	}
}

void intel_slpc_init(struct drm_i915_private *dev_priv)
{
	struct intel_guc *guc = &dev_priv->guc;
	struct i915_vma *vma;

	/* Allocate shared data structure */
	vma = dev_priv->guc.slpc.vma;
	if (!vma) {
		vma = guc_allocate_vma(guc,
			       PAGE_ALIGN(sizeof(struct slpc_shared_data)));
		if (IS_ERR(vma)) {
			DRM_ERROR("slpc_shared_data allocation failed\n");
			i915.enable_slpc = 0;
			return;
		}

		dev_priv->guc.slpc.vma = vma;
	}

	slpc_shared_data_init(dev_priv);
}

void intel_slpc_cleanup(struct drm_i915_private *dev_priv)
{
	struct intel_guc *guc = &dev_priv->guc;

	/* Release shared data structure */
	mutex_lock(&dev_priv->drm.struct_mutex);
	i915_vma_unpin_and_release(&guc->slpc.vma);
	mutex_unlock(&dev_priv->drm.struct_mutex);
}

void intel_slpc_suspend(struct drm_i915_private *dev_priv)
{
}

void intel_slpc_disable(struct drm_i915_private *dev_priv)
{
}

void intel_slpc_enable(struct drm_i915_private *dev_priv)
{
}
