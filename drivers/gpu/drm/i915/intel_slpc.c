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
