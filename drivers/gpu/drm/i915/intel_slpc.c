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

static void host2guc_slpc(struct drm_i915_private *dev_priv, u32 *data, u32 len)
{
	int ret = i915_guc_action(&dev_priv->guc, data, len);

	if (!ret) {
		ret = I915_READ(SOFT_SCRATCH(1));
		ret &= SLPC_EVENT_STATUS_MASK;
	}

	if (ret)
		DRM_ERROR("event 0x%x status %d\n", (data[1] >> 8), ret);
}

static void host2guc_slpc_reset(struct drm_i915_private *dev_priv)
{
	u32 data[4];
	u32 shared_data_gtt_offset = i915_ggtt_offset(dev_priv->guc.slpc.vma);

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_RESET, 2);
	data[2] = shared_data_gtt_offset;
	data[3] = 0;

	host2guc_slpc(dev_priv, data, 4);
}

static void host2guc_slpc_shutdown(struct drm_i915_private *dev_priv)
{
	u32 data[4];
	u32 shared_data_gtt_offset = i915_ggtt_offset(dev_priv->guc.slpc.vma);

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_SHUTDOWN, 2);
	data[2] = shared_data_gtt_offset;
	data[3] = 0;

	host2guc_slpc(dev_priv, data, 4);
}

static void host2guc_slpc_set_param(struct drm_i915_private *dev_priv,
				    enum slpc_param_id id, u32 value)
{
	u32 data[4];

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_PARAMETER_SET, 2);
	data[2] = (u32) id;
	data[3] = value;

	host2guc_slpc(dev_priv, data, 4);
}

static void host2guc_slpc_unset_param(struct drm_i915_private *dev_priv,
				      enum slpc_param_id id)
{
	u32 data[3];

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_PARAMETER_UNSET, 1);
	data[2] = (u32) id;

	host2guc_slpc(dev_priv, data, 3);
}

void intel_slpc_unset_param(struct drm_i915_private *dev_priv,
			    enum slpc_param_id id)
{
	struct drm_i915_gem_object *obj;
	struct page *page;
	struct slpc_shared_data *data = NULL;

	obj = dev_priv->guc.slpc.vma->obj;
	if (obj) {
		page = i915_gem_object_get_page(obj, 0);
		if (page)
			data = kmap_atomic(page);
	}

	if (data) {
		data->override_parameters_set_bits[id >> 5]
							&= (~(1 << (id % 32)));
		data->override_parameters_values[id] = 0;
		kunmap_atomic(data);

		host2guc_slpc_unset_param(dev_priv, id);
	}
}

void intel_slpc_set_param(struct drm_i915_private *dev_priv,
			  enum slpc_param_id id,
			  u32 value)
{
	struct drm_i915_gem_object *obj;
	struct page *page;
	struct slpc_shared_data *data = NULL;

	obj = dev_priv->guc.slpc.vma->obj;
	if (obj) {
		page = i915_gem_object_get_page(obj, 0);
		if (page)
			data = kmap_atomic(page);
	}

	if (data) {
		data->override_parameters_set_bits[id >> 5]
							|= (1 << (id % 32));
		data->override_parameters_values[id] = value;
		kunmap_atomic(data);

		host2guc_slpc_set_param(dev_priv, id, value);
	}
}

void intel_slpc_get_param(struct drm_i915_private *dev_priv,
			  enum slpc_param_id id,
			  int *overriding, u32 *value)
{
	struct drm_i915_gem_object *obj;
	struct page *page;
	struct slpc_shared_data *data = NULL;
	u32 bits;

	obj = dev_priv->guc.slpc.vma->obj;
	if (obj) {
		page = i915_gem_object_get_page(obj, 0);
		if (page)
			data = kmap_atomic(page);
	}

	if (data) {
		if (overriding) {
			bits = data->override_parameters_set_bits[id >> 5];
			*overriding = (0 != (bits & (1 << (id % 32))));
		}
		if (value)
			*value = data->override_parameters_values[id];

		kunmap_atomic(data);
	}
}

static void host2guc_slpc_query_task_state(struct drm_i915_private *dev_priv)
{
	u32 data[4];
	u32 shared_data_gtt_offset = i915_ggtt_offset(dev_priv->guc.slpc.vma);

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_QUERY_TASK_STATE, 2);
	data[2] = shared_data_gtt_offset;
	data[3] = 0;

	host2guc_slpc(dev_priv, data, 4);
}

void intel_slpc_query_task_state(struct drm_i915_private *dev_priv)
{
	if (intel_slpc_active(dev_priv))
		host2guc_slpc_query_task_state(dev_priv);
}

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
		slice_count = INTEL_INFO(dev_priv)->slice_total;

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
	host2guc_slpc_shutdown(dev_priv);
	dev_priv->guc.slpc.enabled = false;
}

void intel_slpc_disable(struct drm_i915_private *dev_priv)
{
	host2guc_slpc_shutdown(dev_priv);
	dev_priv->guc.slpc.enabled = false;
}

void intel_slpc_enable(struct drm_i915_private *dev_priv)
{
	host2guc_slpc_reset(dev_priv);
	DRM_INFO("SLPC Enabled\n");
	dev_priv->guc.slpc.enabled = true;
}
