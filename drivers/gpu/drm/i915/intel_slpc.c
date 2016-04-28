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

static u8 slpc_get_platform_sku(struct drm_i915_gem_object *obj)
{
	struct drm_device *dev = obj->base.dev;
	enum slpc_platform_sku platform_sku;

	if (IS_SKL_ULX(dev))
		platform_sku = SLPC_PLATFORM_SKU_ULX;
	else if (IS_SKL_ULT(dev))
		platform_sku = SLPC_PLATFORM_SKU_ULT;
	else
		platform_sku = SLPC_PLATFORM_SKU_DT;

	return (u8) platform_sku;
}

static u8 slpc_get_slice_count(struct drm_i915_gem_object *obj)
{
	struct drm_device *dev = obj->base.dev;
	u8 slice_count = 1;

	if (IS_SKYLAKE(dev))
		slice_count = INTEL_INFO(dev)->slice_total;

	return slice_count;
}

static void slpc_shared_data_init(struct drm_i915_gem_object *obj)
{
	struct page *page;
	struct slpc_shared_data *data;
	u64 msr_value;

	page = i915_gem_object_get_page(obj, 0);
	if (page) {
		data = kmap_atomic(page);
		memset(data, 0, sizeof(struct slpc_shared_data));

		data->slpc_version = SLPC_VERSION;
		data->shared_data_size = sizeof(struct slpc_shared_data);
		data->global_state = (u32) SLPC_GLOBAL_STATE_NOT_RUNNING;
		data->platform_info.platform_sku = slpc_get_platform_sku(obj);
		data->platform_info.slice_count = slpc_get_slice_count(obj);
		data->platform_info.host_os = (u8) SLPC_HOST_OS_WINDOWS_8;
		data->platform_info.power_plan_source =
			(u8) SLPC_POWER_PLAN_SOURCE(SLPC_POWER_PLAN_BALANCED,
						    SLPC_POWER_SOURCE_AC);
		rdmsrl(MSR_TURBO_RATIO_LIMIT, msr_value);
		data->platform_info.P0_freq = (u8) msr_value;
		rdmsrl(MSR_PLATFORM_INFO, msr_value);
		data->platform_info.P1_freq = (u8) (msr_value >> 8);
		data->platform_info.Pe_freq = (u8) (msr_value >> 40);
		data->platform_info.Pn_freq = (u8) (msr_value >> 48);
		rdmsrl(MSR_PKG_POWER_LIMIT, msr_value);
		data->platform_info.package_rapl_limit_high =
							(u32) (msr_value >> 32);
		data->platform_info.package_rapl_limit_low = (u32) msr_value;

		kunmap_atomic(data);
	}
}

void intel_slpc_init(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj;

	/* Initialize the rps frequecny values */
	mutex_lock(&dev_priv->rps.hw_lock);
	gen6_init_rps_frequencies(dev);
	mutex_unlock(&dev_priv->rps.hw_lock);

	/* Allocate shared data structure */
	obj = dev_priv->guc.slpc.shared_data_obj;
	if (!obj) {
		obj = gem_allocate_guc_obj(dev_priv->dev,
				PAGE_ALIGN(sizeof(struct slpc_shared_data)));
		dev_priv->guc.slpc.shared_data_obj = obj;
	}

	if (!obj)
		DRM_ERROR("slpc_shared_data allocation failed\n");
	else
		slpc_shared_data_init(obj);
}

void intel_slpc_cleanup(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;

	/* Release shared data structure */
	gem_release_guc_obj(dev_priv->guc.slpc.shared_data_obj);
	dev_priv->guc.slpc.shared_data_obj = NULL;
}

void intel_slpc_suspend(struct drm_device *dev)
{
	return;
}

void intel_slpc_disable(struct drm_device *dev)
{
	return;
}

void intel_slpc_enable(struct drm_device *dev)
{
	return;
}

void intel_slpc_reset(struct drm_device *dev)
{
	return;
}
