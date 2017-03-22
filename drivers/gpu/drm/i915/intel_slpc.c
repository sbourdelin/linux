/*
 * Copyright © 2017 Intel Corporation
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
#include "i915_drv.h"
#include "intel_uc.h"

struct slpc_param slpc_paramlist[SLPC_MAX_PARAM] = {
	{SLPC_PARAM_TASK_ENABLE_GTPERF, "Enable task GTPERF"},
	{SLPC_PARAM_TASK_DISABLE_GTPERF, "Disable task GTPERF"},
	{SLPC_PARAM_TASK_ENABLE_BALANCER, "Enable task BALANCER"},
	{SLPC_PARAM_TASK_DISABLE_BALANCER, "Disable task BALANCER"},
	{SLPC_PARAM_TASK_ENABLE_DCC, "Enable task DCC"},
	{SLPC_PARAM_TASK_DISABLE_DCC, "Disable task DCC"},
	{SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ,
				"Minimum GT frequency request for unslice"},
	{SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ,
				"Maximum GT frequency request for unslice"},
	{SLPC_PARAM_GLOBAL_MIN_GT_SLICE_FREQ_MHZ,
				"Minimum GT frequency request for slice"},
	{SLPC_PARAM_GLOBAL_MAX_GT_SLICE_FREQ_MHZ,
				"Maximum GT frequency request for slice"},
	{SLPC_PARAM_GTPERF_THRESHOLD_MAX_FPS,
				"If non-zero, algorithm will slow down "
				"frame-based applications to this frame-rate"},
	{SLPC_PARAM_GLOBAL_DISABLE_GT_FREQ_MANAGEMENT,
				"Lock GT frequency request to RPe"},
	{SLPC_PARAM_GTPERF_ENABLE_FRAMERATE_STALLING,
				"Set to TRUE to enable slowing framerate"},
	{SLPC_PARAM_GLOBAL_DISABLE_RC6_MODE_CHANGE,
				"Prevent from changing the RC mode"},
	{SLPC_PARAM_GLOBAL_OC_UNSLICE_FREQ_MHZ,
				"Override fused value of unslice RP0"},
	{SLPC_PARAM_GLOBAL_OC_SLICE_FREQ_MHZ,
				"Override fused value of slice RP0"},
	{SLPC_PARAM_GLOBAL_ENABLE_IA_GT_BALANCING,
				"TRUE means enable Intelligent Bias Control"},
	{SLPC_PARAM_GLOBAL_ENABLE_ADAPTIVE_BURST_TURBO,
				"TRUE = enable eval mode when transitioning "
				"from idle to active."},
	{SLPC_PARAM_GLOBAL_ENABLE_EVAL_MODE,
				"FALSE = disable eval mode completely"},
	{SLPC_PARAM_GLOBAL_ENABLE_BALANCER_IN_NON_GAMING_MODE,
				"Enable IBC when non-Gaming Mode is enabled"}
};

static void host2guc_slpc(struct drm_i915_private *dev_priv,
			  struct slpc_event_input *input, u32 len)
{
	u32 *data;
	u32 output[SLPC_EVENT_MAX_OUTPUT_ARGS];
	int ret = 0;

	/*
	 * We have only 15 scratch registers for communication.
	 * the first we will use for the event ID in input and
	 * output data. Event processing status will be present
	 * in SOFT_SCRATCH(1) register.
	 */
	BUILD_BUG_ON(SLPC_EVENT_MAX_INPUT_ARGS > 14);
	BUILD_BUG_ON(SLPC_EVENT_MAX_OUTPUT_ARGS < 1);
	BUILD_BUG_ON(SLPC_EVENT_MAX_OUTPUT_ARGS > 14);

	data = (u32 *) input;
	data[0] = INTEL_GUC_ACTION_SLPC_REQUEST;
	ret = __intel_guc_send(&dev_priv->guc, data, len, output);

	if (ret)
		DRM_ERROR("event 0x%x status %d\n",
			  ((output[0] & 0xFF00) >> 8), ret);
}

void slpc_mem_set_param(struct slpc_shared_data *data,
			      u32 id,
			      u32 value)
{
	data->override_parameters_set_bits[id >> 5]
						|= (1 << (id % 32));
	data->override_parameters_values[id] = value;
}

void slpc_mem_unset_param(struct slpc_shared_data *data,
				u32 id)
{
	data->override_parameters_set_bits[id >> 5]
						&= (~(1 << (id % 32)));
	data->override_parameters_values[id] = 0;
}

static void host2guc_slpc_set_param(struct drm_i915_private *dev_priv,
				    u32 id, u32 value)
{
	struct slpc_event_input data = {0};

	data.header.value = SLPC_EVENT(SLPC_EVENT_PARAMETER_SET, 2);
	data.args[0] = id;
	data.args[1] = value;

	host2guc_slpc(dev_priv, &data, 4);
}

static void host2guc_slpc_unset_param(struct drm_i915_private *dev_priv,
				      u32 id)
{
	struct slpc_event_input data = {0};

	data.header.value = SLPC_EVENT(SLPC_EVENT_PARAMETER_UNSET, 1);
	data.args[0] = id;

	host2guc_slpc(dev_priv, &data, 3);
}

void intel_slpc_set_param(struct drm_i915_private *dev_priv,
			  u32 id,
			  u32 value)
{
	struct page *page;
	struct slpc_shared_data *data = NULL;

	WARN_ON(id >= SLPC_MAX_PARAM);

	if (!dev_priv->guc.slpc.vma)
		return;

	page = i915_vma_first_page(dev_priv->guc.slpc.vma);
	data = kmap_atomic(page);
	slpc_mem_set_param(data, id, value);
	kunmap_atomic(data);

	host2guc_slpc_set_param(dev_priv, id, value);
}

void intel_slpc_unset_param(struct drm_i915_private *dev_priv,
			    u32 id)
{
	struct page *page;
	struct slpc_shared_data *data = NULL;

	WARN_ON(id >= SLPC_MAX_PARAM);

	if (!dev_priv->guc.slpc.vma)
		return;

	page = i915_vma_first_page(dev_priv->guc.slpc.vma);
	data = kmap_atomic(page);
	slpc_mem_unset_param(data, id);
	kunmap_atomic(data);

	host2guc_slpc_unset_param(dev_priv, id);
}

void intel_slpc_get_param(struct drm_i915_private *dev_priv,
			  u32 id,
			  int *overriding, u32 *value)
{
	struct page *page;
	struct slpc_shared_data *data = NULL;
	u32 bits;

	WARN_ON(id >= SLPC_MAX_PARAM);

	if (!dev_priv->guc.slpc.vma)
		return;

	page = i915_vma_first_page(dev_priv->guc.slpc.vma);
	data = kmap_atomic(page);
	if (overriding) {
		bits = data->override_parameters_set_bits[id >> 5];
		*overriding = (0 != (bits & (1 << (id % 32))));
	}
	if (value)
		*value = data->override_parameters_values[id];

	kunmap_atomic(data);
}

int slpc_mem_task_control(struct slpc_shared_data *data, u64 val,
			  u32 enable_id, u32 disable_id)
{
	int ret = 0;

	if (val == SLPC_PARAM_TASK_DEFAULT) {
		/* set default */
		slpc_mem_unset_param(data, enable_id);
		slpc_mem_unset_param(data, disable_id);
	} else if (val == SLPC_PARAM_TASK_ENABLED) {
		/* set enable */
		slpc_mem_set_param(data, enable_id, 1);
		slpc_mem_unset_param(data, disable_id);
	} else if (val == SLPC_PARAM_TASK_DISABLED) {
		/* set disable */
		slpc_mem_set_param(data, disable_id, 1);
		slpc_mem_unset_param(data, enable_id);
	} else {
		ret = -EINVAL;
	}

	return ret;
}

int intel_slpc_task_control(struct drm_i915_private *dev_priv, u64 val,
			    u32 enable_id, u32 disable_id)
{
	int ret = 0;

	if (!dev_priv->guc.slpc.active)
		return -ENODEV;

	intel_runtime_pm_get(dev_priv);

	if (val == SLPC_PARAM_TASK_DEFAULT) {
		/* set default */
		intel_slpc_unset_param(dev_priv, enable_id);
		intel_slpc_unset_param(dev_priv, disable_id);
	} else if (val == SLPC_PARAM_TASK_ENABLED) {
		/* set enable */
		intel_slpc_set_param(dev_priv, enable_id, 1);
		intel_slpc_unset_param(dev_priv, disable_id);
	} else if (val == SLPC_PARAM_TASK_DISABLED) {
		/* set disable */
		intel_slpc_set_param(dev_priv, disable_id, 1);
		intel_slpc_unset_param(dev_priv, enable_id);
	} else {
		ret = -EINVAL;
	}

	intel_slpc_enable(dev_priv);
	intel_runtime_pm_put(dev_priv);

	return ret;
}

int intel_slpc_task_status(struct drm_i915_private *dev_priv, u64 *val,
			   u32 enable_id, u32 disable_id)
{
	int override_enable, override_disable;
	u32 value_enable, value_disable;
	int ret = 0;

	if (!dev_priv->guc.slpc.active) {
		ret = -ENODEV;
	} else if (val) {
		intel_slpc_get_param(dev_priv, enable_id, &override_enable,
				     &value_enable);
		intel_slpc_get_param(dev_priv, disable_id, &override_disable,
				     &value_disable);

		/*
		 * Set the output value:
		 * 0: default
		 * 1: enabled
		 * 2: disabled
		 * 3: unknown (should not happen)
		 */
		if (override_disable && (value_disable == 1))
			*val = SLPC_PARAM_TASK_DISABLED;
		else if (override_enable && (value_enable == 1))
			*val = SLPC_PARAM_TASK_ENABLED;
		else if (!override_enable && !override_disable)
			*val = SLPC_PARAM_TASK_DEFAULT;
		else
			*val = SLPC_PARAM_TASK_UNKNOWN;

	} else {
		ret = -EINVAL;
	}

	return ret;
}

void intel_slpc_init(struct drm_i915_private *dev_priv)
{
}

void intel_slpc_cleanup(struct drm_i915_private *dev_priv)
{
}

void intel_slpc_enable(struct drm_i915_private *dev_priv)
{
}

void intel_slpc_disable(struct drm_i915_private *dev_priv)
{
}
