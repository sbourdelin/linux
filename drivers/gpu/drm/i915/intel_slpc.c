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
	int ret = host2guc_action(&dev_priv->guc, data, len);

	if (!ret) {
		ret = I915_READ(SOFT_SCRATCH(1));
		ret &= SLPC_EVENT_STATUS_MASK;
	}

	if (ret)
		DRM_ERROR("event 0x%x status %d\n", (data[1] >> 8), ret);
}

static void host2guc_slpc_reset(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj = dev_priv->guc.slpc.shared_data_obj;
	u32 data[4];
	u64 shared_data_gtt_offset = i915_gem_obj_ggtt_offset(obj);

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_RESET, 2);
	data[2] = lower_32_bits(shared_data_gtt_offset);
	data[3] = upper_32_bits(shared_data_gtt_offset);

	WARN_ON(data[3] != 0);

	host2guc_slpc(dev_priv, data, 4);
}

static void host2guc_slpc_shutdown(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj = dev_priv->guc.slpc.shared_data_obj;
	u32 data[4];
	u64 shared_data_gtt_offset = i915_gem_obj_ggtt_offset(obj);

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_SHUTDOWN, 2);
	data[2] = lower_32_bits(shared_data_gtt_offset);
	data[3] = upper_32_bits(shared_data_gtt_offset);

	WARN_ON(0 != data[3]);

	host2guc_slpc(dev_priv, data, 4);
}

static void host2guc_slpc_display_mode_change(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	u32 data[3 + SLPC_MAX_NUM_OF_PIPES];
	int i;
	struct intel_slpc_display_mode_event_params *display_mode_params;

	display_mode_params = &dev_priv->guc.slpc.display_mode_params;
	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_DISPLAY_MODE_CHANGE,
					SLPC_MAX_NUM_OF_PIPES + 1);
	data[2] = display_mode_params->global_data;
	for(i = 0; i < SLPC_MAX_NUM_OF_PIPES; ++i)
		data[3+i] = display_mode_params->per_pipe_info[i].data;

	host2guc_slpc(dev_priv, data, 3 + SLPC_MAX_NUM_OF_PIPES);
}

static void host2guc_slpc_set_param(struct drm_device *dev,
				    enum slpc_param_id id, u32 value)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	u32 data[4];

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_PARAMETER_SET, 2);
	data[2] = (u32) id;
	data[3] = value;

	host2guc_slpc(dev_priv, data, 4);
}

static void host2guc_slpc_unset_param(struct drm_device *dev,
				      enum slpc_param_id id)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	u32 data[3];

	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	data[1] = SLPC_EVENT(SLPC_EVENT_PARAMETER_UNSET, 1);
	data[2] = (u32) id;

	host2guc_slpc(dev_priv, data, 3);
}

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
	if (intel_slpc_active(dev))
		host2guc_slpc_shutdown(dev);
}

void intel_slpc_disable(struct drm_device *dev)
{
	if (intel_slpc_active(dev))
		host2guc_slpc_shutdown(dev);
}

void intel_slpc_enable(struct drm_device *dev)
{
	if (intel_slpc_active(dev)) {
		host2guc_slpc_reset(dev);
		intel_slpc_update_display_mode_info(dev);
	}
}

void intel_slpc_reset(struct drm_device *dev)
{
	if (intel_slpc_active(dev)) {
		host2guc_slpc_shutdown(dev);
		host2guc_slpc_reset(dev);
	}
}

void intel_slpc_update_display_mode_info(struct drm_device *dev)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc;
	struct intel_display_pipe_info *per_pipe_info;
	struct intel_slpc_display_mode_event_params *cur_params, old_params;
	bool notify = false;

	if (!intel_slpc_active(dev))
		return;

	/* Copy display mode parameters for comparison */
	cur_params = &dev_priv->guc.slpc.display_mode_params;
	old_params.global_data  = cur_params->global_data;
	cur_params->global_data = 0;

	intel_runtime_pm_get(dev_priv);
	drm_modeset_lock_all(dev);

	for_each_intel_crtc(dev, intel_crtc) {
		per_pipe_info = &cur_params->per_pipe_info[intel_crtc->pipe];
		old_params.per_pipe_info[intel_crtc->pipe].data =
							per_pipe_info->data;
		per_pipe_info->data = 0;

		if (intel_crtc->active) {
			struct drm_display_mode *mode = &intel_crtc->base.mode;

			if (mode->clock == 0 || mode->htotal == 0 ||
			    mode->vtotal == 0) {
				DRM_DEBUG_DRIVER(
					"Display Mode Info not sent to SLPC\n");
				drm_modeset_unlock_all(dev);
				intel_runtime_pm_put(dev_priv);
				return;
			}
			/* FIXME: Update is_widi based on encoder */
			per_pipe_info->is_widi = 0;
			per_pipe_info->refresh_rate =
						(mode->clock * 1000) /
						(mode->htotal * mode->vtotal);
			per_pipe_info->vsync_ft_usec =
					(mode->htotal * mode->vtotal * 1000) /
						mode->clock;
			cur_params->active_pipes_bitmask |=
							(1 << intel_crtc->pipe);
			cur_params->vbi_sync_on_pipes |=
							(1 << intel_crtc->pipe);
		} else {
			cur_params->active_pipes_bitmask &=
						~(1 << intel_crtc->pipe);
			cur_params->vbi_sync_on_pipes &=
						~(1 << intel_crtc->pipe);
		}

		if (old_params.per_pipe_info[intel_crtc->pipe].data !=
							per_pipe_info->data)
			notify = true;
	}

	drm_modeset_unlock_all(dev);

	cur_params->num_active_pipes =
				hweight32(cur_params->active_pipes_bitmask);

	/*
	 * Compare old display mode with current mode.
	 * Notify SLPC if it is changed.
	*/
	if (cur_params->global_data != old_params.global_data)
		notify = true;

	if (notify)
		host2guc_slpc_display_mode_change(dev);

	intel_runtime_pm_put(dev_priv);
}

void intel_slpc_update_atomic_commit_info(struct drm_device *dev,
					  struct drm_atomic_state *state)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_crtc *crtc;
	struct drm_crtc_state *crtc_state;
	struct intel_display_pipe_info *per_pipe_info;
	struct intel_slpc_display_mode_event_params *cur_params, old_params;
	bool notify = false;
	int i;

	if (!intel_slpc_active(dev))
		return;

	/* Copy display mode parameters for comparison */
	cur_params = &dev_priv->guc.slpc.display_mode_params;
	old_params.global_data  = cur_params->global_data;

	for_each_crtc_in_state(state, crtc, crtc_state, i) {
		struct intel_crtc *intel_crtc = to_intel_crtc(crtc);

		per_pipe_info = &cur_params->per_pipe_info[intel_crtc->pipe];
		old_params.per_pipe_info[intel_crtc->pipe].data =
							per_pipe_info->data;

		per_pipe_info->data = 0;
		cur_params->active_pipes_bitmask &=
						~(1 << intel_crtc->pipe);
		cur_params->vbi_sync_on_pipes &=
						~(1 << intel_crtc->pipe);

		if (crtc_state->active) {
			struct drm_display_mode *mode = &crtc->mode;

			if (mode->clock == 0 || mode->htotal == 0 ||
			    mode->vtotal == 0) {
				DRM_DEBUG_DRIVER(
					"Display Mode Info not sent to SLPC\n");
				return;
			}

			/* FIXME: Update is_widi based on encoder */
			per_pipe_info->is_widi = 0;
			per_pipe_info->refresh_rate =
						(mode->clock * 1000) /
						(mode->htotal * mode->vtotal);
			per_pipe_info->vsync_ft_usec =
					(mode->htotal * mode->vtotal * 1000) /
						mode->clock;
			cur_params->active_pipes_bitmask |=
							(1 << intel_crtc->pipe);
			cur_params->vbi_sync_on_pipes |=
							(1 << intel_crtc->pipe);
		}

		if (old_params.per_pipe_info[intel_crtc->pipe].data !=
							per_pipe_info->data)
			notify = true;
	}

	cur_params->num_active_pipes =
				hweight32(cur_params->active_pipes_bitmask);

	/*
	 * Compare old display mode with current mode.
	 * Notify SLPC if it is changed.
	*/
	if (cur_params->global_data != old_params.global_data)
		notify = true;

	if (notify)
		host2guc_slpc_display_mode_change(dev);
}

void intel_slpc_update_display_rr_info(struct drm_device *dev, u32 refresh_rate)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_crtc *crtc;
	struct intel_display_pipe_info *per_pipe_info;
	struct intel_slpc_display_mode_event_params *display_params;

	if (!intel_slpc_active(dev))
		return;

	if (!refresh_rate)
		return;

	display_params = &dev_priv->guc.slpc.display_mode_params;
	crtc = dp_to_dig_port(dev_priv->drrs.dp)->base.base.crtc;

	per_pipe_info = &display_params->per_pipe_info[to_intel_crtc(crtc)->pipe];
	per_pipe_info->refresh_rate = refresh_rate;
	per_pipe_info->vsync_ft_usec = 1000000 / refresh_rate;

	host2guc_slpc_display_mode_change(dev);
}

void intel_slpc_unset_param(struct drm_device *dev, enum slpc_param_id id)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj;
	struct page *page;
	struct slpc_shared_data *data = NULL;

	obj = dev_priv->guc.slpc.shared_data_obj;
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

		host2guc_slpc_unset_param(dev, id);
	}
}

void intel_slpc_set_param(struct drm_device *dev, enum slpc_param_id id,
			  u32 value)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj;
	struct page *page;
	struct slpc_shared_data *data = NULL;

	obj = dev_priv->guc.slpc.shared_data_obj;
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

		host2guc_slpc_set_param(dev, id, value);
	}
}

void intel_slpc_get_param(struct drm_device *dev, enum slpc_param_id id,
			  int *overriding, u32 *value)
{
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_i915_gem_object *obj;
	struct page *page;
	struct slpc_shared_data *data = NULL;
	u32 bits;

	obj = dev_priv->guc.slpc.shared_data_obj;
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
