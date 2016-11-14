/*
 * Copyright © 2016 Intel Corporation
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
	struct page *page;
	struct slpc_shared_data *data;
	u64 msr_value;

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
	rdmsrl(MSR_TURBO_RATIO_LIMIT, msr_value);
	data->platform_info.P0_freq = msr_value;
	rdmsrl(MSR_PLATFORM_INFO, msr_value);
	data->platform_info.P1_freq = msr_value >> 8;
	data->platform_info.Pe_freq = msr_value >> 40;
	data->platform_info.Pn_freq = msr_value >> 48;

	kunmap_atomic(data);
}

static void host2guc_slpc(struct drm_i915_private *dev_priv,
			  struct slpc_event_input *input, u32 len)
{
	union slpc_event_output_header header;
	u32 *data;
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
	data[0] = HOST2GUC_ACTION_SLPC_REQUEST;
	ret = i915_guc_action(&dev_priv->guc, data, len);

	if (!ret) {
		header.value = I915_READ(SOFT_SCRATCH(1));
		ret = header.status;
	}

	if (ret)
		DRM_ERROR("event 0x%x status %d\n", (data[1] >> 8), ret);
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

void slpc_mem_set_param(struct slpc_shared_data *data,
			      u32 id,
			      u32 value)
{
	data->override_parameters_set_bits[id >> 5]
						|= (1 << (id % 32));
	data->override_parameters_values[id] = value;
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

static void host2guc_slpc_unset_param(struct drm_i915_private *dev_priv,
				      u32 id)
{
	struct slpc_event_input data = {0};

	data.header.value = SLPC_EVENT(SLPC_EVENT_PARAMETER_UNSET, 1);
	data.args[0] = id;

	host2guc_slpc(dev_priv, &data, 3);
}

void slpc_mem_unset_param(struct slpc_shared_data *data,
				u32 id)
{
	data->override_parameters_set_bits[id >> 5]
						&= (~(1 << (id % 32)));
	data->override_parameters_values[id] = 0;
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

void intel_slpc_init(struct drm_i915_private *dev_priv)
{
	struct intel_guc *guc = &dev_priv->guc;
	struct i915_vma *vma;

	dev_priv->guc.slpc.active = false;

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

static void host2guc_slpc_shutdown(struct drm_i915_private *dev_priv)
{
	struct slpc_event_input data = {0};
	u32 shared_data_gtt_offset = i915_ggtt_offset(dev_priv->guc.slpc.vma);

	data.header.value = SLPC_EVENT(SLPC_EVENT_SHUTDOWN, 2);
	data.args[0] = shared_data_gtt_offset;
	data.args[1] = 0;

	host2guc_slpc(dev_priv, &data, 4);
}

void intel_slpc_suspend(struct drm_i915_private *dev_priv)
{
	host2guc_slpc_shutdown(dev_priv);
	dev_priv->guc.slpc.active = false;
}

void intel_slpc_disable(struct drm_i915_private *dev_priv)
{
	host2guc_slpc_shutdown(dev_priv);
	dev_priv->guc.slpc.active = false;
}

static void host2guc_slpc_reset(struct drm_i915_private *dev_priv)
{
	struct slpc_event_input data = {0};
	u32 shared_data_gtt_offset = i915_ggtt_offset(dev_priv->guc.slpc.vma);

	data.header.value = SLPC_EVENT(SLPC_EVENT_RESET, 2);
	data.args[0] = shared_data_gtt_offset;
	data.args[1] = 0;

	host2guc_slpc(dev_priv, &data, 4);
}

void intel_slpc_enable(struct drm_i915_private *dev_priv)
{
	host2guc_slpc_reset(dev_priv);
	dev_priv->guc.slpc.active = true;
}

static void host2guc_slpc_query_task_state(struct drm_i915_private *dev_priv)
{
	struct slpc_event_input data = {0};
	u32 shared_data_gtt_offset = i915_ggtt_offset(dev_priv->guc.slpc.vma);

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

	drm_clflush_virt_range(pv, PAGE_SIZE);
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
 * TODO: Add separate interfaces to set Max/Min Slice frequency.
 * Since currently both slice and unslice are configured to same
 * frequencies these unified interface relying on Unslice frequencies
 * should be sufficient. These functions take frequency opcode as input.
 */
int intel_slpc_max_freq_set(struct drm_i915_private *dev_priv, u32 val)
{
	if (val < dev_priv->rps.min_freq ||
	    val > dev_priv->rps.max_freq ||
	    val < dev_priv->guc.slpc.min_unslice_freq)
		return -EINVAL;

	intel_slpc_set_param(dev_priv,
			     SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ,
			     intel_gpu_freq(dev_priv, val));
	intel_slpc_set_param(dev_priv,
			     SLPC_PARAM_GLOBAL_MAX_GT_SLICE_FREQ_MHZ,
			     intel_gpu_freq(dev_priv, val));

	dev_priv->guc.slpc.max_unslice_freq = val;

	return 0;
}

int intel_slpc_min_freq_set(struct drm_i915_private *dev_priv, u32 val)
{
	if (val < dev_priv->rps.min_freq ||
	    val > dev_priv->rps.max_freq ||
	    val > dev_priv->guc.slpc.max_unslice_freq)
		return -EINVAL;

	intel_slpc_set_param(dev_priv,
			     SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ,
			     intel_gpu_freq(dev_priv, val));
	intel_slpc_set_param(dev_priv,
			     SLPC_PARAM_GLOBAL_MIN_GT_SLICE_FREQ_MHZ,
			     intel_gpu_freq(dev_priv, val));

	dev_priv->guc.slpc.min_unslice_freq = val;

	return 0;
}
