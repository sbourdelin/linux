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
#include <asm/msr-index.h>
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
	struct page *page;
	struct slpc_shared_data *data;
	u64 val;

	page = i915_vma_first_page(dev_priv->guc.slpc.vma);
	data = kmap_atomic(page);

	memset(data, 0, sizeof(struct slpc_shared_data));

	data->shared_data_size = sizeof(struct slpc_shared_data);
	data->global_state = SLPC_GLOBAL_STATE_NOT_RUNNING;
	data->platform_info.platform_sku =
				slpc_get_platform_sku(dev_priv);
	data->platform_info.slice_count =
				slpc_get_slice_count(dev_priv);
	data->platform_info.power_plan_source =
		SLPC_POWER_PLAN_SOURCE(SLPC_POWER_PLAN_BALANCED,
					    SLPC_POWER_SOURCE_AC);
	rdmsrl(MSR_TURBO_RATIO_LIMIT, val);
	data->platform_info.P0_freq = val;
	rdmsrl(MSR_PLATFORM_INFO, val);
	data->platform_info.P1_freq = val >> 8;
	data->platform_info.Pe_freq = val >> 40;
	data->platform_info.Pn_freq = val >> 48;

	/* Enable only GTPERF task, Disable others */
	val = SLPC_PARAM_TASK_ENABLED;
	slpc_mem_task_control(data, val,
			      SLPC_PARAM_TASK_ENABLE_GTPERF,
			      SLPC_PARAM_TASK_DISABLE_GTPERF);

	val = SLPC_PARAM_TASK_DISABLED;
	slpc_mem_task_control(data, val,
			      SLPC_PARAM_TASK_ENABLE_BALANCER,
			      SLPC_PARAM_TASK_DISABLE_BALANCER);

	slpc_mem_task_control(data, val,
			      SLPC_PARAM_TASK_ENABLE_DCC,
			      SLPC_PARAM_TASK_DISABLE_DCC);

	slpc_mem_set_param(data, SLPC_PARAM_GTPERF_THRESHOLD_MAX_FPS, 0);

	slpc_mem_set_param(data, SLPC_PARAM_GTPERF_ENABLE_FRAMERATE_STALLING,
			   0);

	slpc_mem_set_param(data, SLPC_PARAM_GLOBAL_ENABLE_IA_GT_BALANCING,
			   0);

	slpc_mem_set_param(data,
			   SLPC_PARAM_GLOBAL_ENABLE_ADAPTIVE_BURST_TURBO,
			   0);

	slpc_mem_set_param(data, SLPC_PARAM_GLOBAL_ENABLE_EVAL_MODE, 0);

	slpc_mem_set_param(data,
			   SLPC_PARAM_GLOBAL_ENABLE_BALANCER_IN_NON_GAMING_MODE,
			   0);

	slpc_mem_set_param(data,
			   SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ,
			   intel_gpu_freq(dev_priv,
				dev_priv->rps.efficient_freq));
	slpc_mem_set_param(data,
			   SLPC_PARAM_GLOBAL_MIN_GT_SLICE_FREQ_MHZ,
			   intel_gpu_freq(dev_priv,
				dev_priv->rps.efficient_freq));

	kunmap_atomic(data);
}

static void host2guc_slpc_reset(struct drm_i915_private *dev_priv)
{
	struct slpc_event_input data = {0};
	u32 shared_data_gtt_offset = guc_ggtt_offset(dev_priv->guc.slpc.vma);

	data.header.value = SLPC_EVENT(SLPC_EVENT_RESET, 2);
	data.args[0] = shared_data_gtt_offset;
	data.args[1] = 0;

	host2guc_slpc(dev_priv, &data, 4);
}

static void host2guc_slpc_query_task_state(struct drm_i915_private *dev_priv)
{
	struct slpc_event_input data = {0};
	u32 shared_data_gtt_offset = guc_ggtt_offset(dev_priv->guc.slpc.vma);

	data.header.value = SLPC_EVENT(SLPC_EVENT_QUERY_TASK_STATE, 2);
	data.args[0] = shared_data_gtt_offset;
	data.args[1] = 0;

	host2guc_slpc(dev_priv, &data, 4);
}

void intel_slpc_query_task_state(struct drm_i915_private *dev_priv)
{
	if (dev_priv->guc.slpc.active)
		host2guc_slpc_query_task_state(dev_priv);
}

/*
 * This function will reads the state updates from GuC SLPC into shared data
 * by invoking H2G action. Returns current state of GuC SLPC.
 */
void intel_slpc_read_shared_data(struct drm_i915_private *dev_priv,
				 struct slpc_shared_data *data)
{
	struct page *page;
	void *pv = NULL;

	intel_slpc_query_task_state(dev_priv);

	page = i915_vma_first_page(dev_priv->guc.slpc.vma);
	pv = kmap_atomic(page);

	drm_clflush_virt_range(pv, sizeof(struct slpc_shared_data));
	memcpy(data, pv, sizeof(struct slpc_shared_data));

	kunmap_atomic(pv);
}

const char *intel_slpc_get_state_str(enum slpc_global_state state)
{
	if (state == SLPC_GLOBAL_STATE_NOT_RUNNING)
		return "not running";
	else if (state == SLPC_GLOBAL_STATE_INITIALIZING)
		return "initializing";
	else if (state == SLPC_GLOBAL_STATE_RESETTING)
		return "resetting";
	else if (state == SLPC_GLOBAL_STATE_RUNNING)
		return "running";
	else if (state == SLPC_GLOBAL_STATE_SHUTTING_DOWN)
		return "shutting down";
	else if (state == SLPC_GLOBAL_STATE_ERROR)
		return "error";
	else
		return "unknown";
}
bool intel_slpc_get_status(struct drm_i915_private *dev_priv)
{
	struct slpc_shared_data data;
	bool ret = false;

	intel_slpc_read_shared_data(dev_priv, &data);
	DRM_INFO("SLPC state: %s\n",
		 intel_slpc_get_state_str(data.global_state));

	switch (data.global_state) {
	case SLPC_GLOBAL_STATE_RUNNING:
		/* Capture required state from SLPC here */
		ret = true;
		break;
	case SLPC_GLOBAL_STATE_ERROR:
		DRM_ERROR("SLPC in error state.\n");
		break;
	case SLPC_GLOBAL_STATE_RESETTING:
		/*
		 * SLPC enabling in GuC should be completing fast.
		 * If SLPC is taking time to initialize (unlikely as we are
		 * sending reset event during GuC load itself).
		 * TODO: Need to wait till state changes to RUNNING.
		 */
		ret = true;
		DRM_ERROR("SLPC not running yet.!!!");
		break;
	default:
		break;
	}
	return ret;
}

/*
 * Uncore sanitize clears RPS state in Host GTPM flows set by BIOS, Save the
 * initial BIOS programmed RPS state that is needed by SLPC and not set by SLPC.
 * Set this state while enabling SLPC.
 */
void intel_slpc_save_default_rps(struct drm_i915_private *dev_priv)
{
	dev_priv->guc.slpc.rp_control = I915_READ(GEN6_RP_CONTROL);
}

static void intel_slpc_restore_default_rps(struct drm_i915_private *dev_priv)
{
	I915_WRITE(GEN6_RP_CONTROL, dev_priv->guc.slpc.rp_control);
}

void intel_slpc_init(struct drm_i915_private *dev_priv)
{
	struct intel_guc *guc = &dev_priv->guc;
	struct i915_vma *vma;

	dev_priv->guc.slpc.active = false;

	mutex_lock(&dev_priv->rps.hw_lock);
	gen6_init_rps_frequencies(dev_priv);
	mutex_unlock(&dev_priv->rps.hw_lock);

	/* Allocate shared data structure */
	vma = dev_priv->guc.slpc.vma;
	if (!vma) {
		vma = intel_guc_allocate_vma(guc,
			       PAGE_ALIGN(sizeof(struct slpc_shared_data)));
		if (IS_ERR(vma)) {
			DRM_ERROR("slpc_shared_data allocation failed\n");
			i915.enable_slpc = 0;
			return;
		}

		dev_priv->guc.slpc.vma = vma;
		slpc_shared_data_init(dev_priv);
	}
}

void intel_slpc_cleanup(struct drm_i915_private *dev_priv)
{
	struct intel_guc *guc = &dev_priv->guc;

	/* Release shared data structure */
	i915_vma_unpin_and_release(&guc->slpc.vma);
}

void intel_slpc_enable(struct drm_i915_private *dev_priv)
{
	struct page *page;
	struct slpc_shared_data *data;

	intel_slpc_restore_default_rps(dev_priv);

	page = i915_vma_first_page(dev_priv->guc.slpc.vma);
	data = kmap_atomic(page);
	data->global_state = SLPC_GLOBAL_STATE_NOT_RUNNING;
	kunmap_atomic(data);

	host2guc_slpc_reset(dev_priv);
	dev_priv->guc.slpc.active = true;
}

void intel_slpc_disable(struct drm_i915_private *dev_priv)
{
}
