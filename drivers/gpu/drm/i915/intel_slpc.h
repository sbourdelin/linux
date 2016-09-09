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
#ifndef _INTEL_SLPC_H_
#define _INTEL_SLPC_H_

enum slpc_status {
	SLPC_STATUS_OK = 0,
	SLPC_STATUS_ERROR = 1,
	SLPC_STATUS_ILLEGAL_COMMAND = 2,
	SLPC_STATUS_INVALID_ARGS = 3,
	SLPC_STATUS_INVALID_PARAMS = 4,
	SLPC_STATUS_INVALID_DATA = 5,
	SLPC_STATUS_OUT_OF_RANGE = 6,
	SLPC_STATUS_NOT_SUPPORTED = 7,
	SLPC_STATUS_NOT_IMPLEMENTED = 8,
	SLPC_STATUS_NO_DATA = 9,
	SLPC_STATUS_EVENT_NOT_REGISTERED = 10,
	SLPC_STATUS_REGISTER_LOCKED = 11,
	SLPC_STATUS_TEMPORARILY_UNAVAILABLE = 12,
	SLPC_STATUS_VALUE_ALREADY_SET = 13,
	SLPC_STATUS_VALUE_ALREADY_UNSET = 14,
	SLPC_STATUS_VALUE_NOT_CHANGED = 15,
	SLPC_STATUS_MEMIO_ERROR = 16,
	SLPC_STATUS_EVENT_QUEUED_REQ_DPC = 17,
	SLPC_STATUS_EVENT_QUEUED_NOREQ_DPC = 18,
	SLPC_STATUS_NO_EVENT_QUEUED = 19,
	SLPC_STATUS_OUT_OF_SPACE = 20,
	SLPC_STATUS_TIMEOUT = 21,
	SLPC_STATUS_NO_LOCK = 22,
};

enum slpc_event_id {
	SLPC_EVENT_RESET = 0,
	SLPC_EVENT_SHUTDOWN = 1,
	SLPC_EVENT_PLATFORM_INFO_CHANGE = 2,
	SLPC_EVENT_DISPLAY_MODE_CHANGE = 3,
	SLPC_EVENT_FLIP_COMPLETE = 4,
	SLPC_EVENT_QUERY_TASK_STATE = 5,
	SLPC_EVENT_PARAMETER_SET = 6,
	SLPC_EVENT_PARAMETER_UNSET = 7,
};

#define SLPC_EVENT(id, argc) ((u32) (id) << 8 | (argc))
#define SLPC_EVENT_STATUS_MASK	0xFF

enum slpc_param_id {
	SLPC_PARAM_TASK_ENABLE_GTPERF = 0,
	SLPC_PARAM_TASK_DISABLE_GTPERF = 1,
	SLPC_PARAM_TASK_ENABLE_BALANCER = 2,
	SLPC_PARAM_TASK_DISABLE_BALANCER = 3,
	SLPC_PARAM_TASK_ENABLE_DCC = 4,
	SLPC_PARAM_TASK_DISABLE_DCC = 5,
	SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ = 6,
	SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ = 7,
	SLPC_PARAM_GLOBAL_MIN_GT_SLICE_FREQ_MHZ = 8,
	SLPC_PARAM_GLOBAL_MAX_GT_SLICE_FREQ_MHZ = 9,
	SLPC_PARAM_GTPERF_THRESHOLD_MAX_FPS = 10,
	SLPC_PARAM_GLOBAL_DISABLE_GT_FREQ_MANAGEMENT = 11,
	SLPC_PARAM_GTPERF_ENABLE_FRAMERATE_STALLING = 12,
	SLPC_PARAM_GLOBAL_DISABLE_RC6_MODE_CHANGE = 13,
	SLPC_PARAM_GLOBAL_OC_UNSLICE_FREQ_MHZ = 14,
	SLPC_PARAM_GLOBAL_OC_SLICE_FREQ_MHZ = 15,
	SLPC_PARAM_GLOBAL_ENABLE_IA_GT_BALANCING = 16,
	SLPC_PARAM_GLOBAL_ENABLE_ADAPTIVE_BURST_TURBO = 17,
	SLPC_PARAM_GLOBAL_ENABLE_EVAL_MODE = 18,
	SLPC_PARAM_GLOBAL_ENABLE_BALANCER_IN_NON_GAMING_MODE = 19,
};

#define SLPC_PARAM_TASK_DEFAULT 0
#define SLPC_PARAM_TASK_ENABLED 1
#define SLPC_PARAM_TASK_DISABLED 2
#define SLPC_PARAM_TASK_UNKNOWN 3

enum slpc_global_state {
	SLPC_GLOBAL_STATE_NOT_RUNNING = 0,
	SLPC_GLOBAL_STATE_INITIALIZING = 1,
	SLPC_GLOBAL_STATE_RESETTING = 2,
	SLPC_GLOBAL_STATE_RUNNING = 3,
	SLPC_GLOBAL_STATE_SHUTTING_DOWN = 4,
	SLPC_GLOBAL_STATE_ERROR = 5
};

enum slpc_platform_sku {
	SLPC_PLATFORM_SKU_UNDEFINED = 0,
	SLPC_PLATFORM_SKU_ULX = 1,
	SLPC_PLATFORM_SKU_ULT = 2,
	SLPC_PLATFORM_SKU_T = 3,
	SLPC_PLATFORM_SKU_MOBL = 4,
	SLPC_PLATFORM_SKU_DT = 5,
	SLPC_PLATFORM_SKU_UNKNOWN = 6,
};

enum slpc_power_plan {
	SLPC_POWER_PLAN_UNDEFINED = 0,
	SLPC_POWER_PLAN_BATTERY_SAVER = 1,
	SLPC_POWER_PLAN_BALANCED = 2,
	SLPC_POWER_PLAN_PERFORMANCE = 3,
	SLPC_POWER_PLAN_UNKNOWN = 4,
};

enum slpc_power_source {
	SLPC_POWER_SOURCE_UNDEFINED = 0,
	SLPC_POWER_SOURCE_AC = 1,
	SLPC_POWER_SOURCE_DC = 2,
	SLPC_POWER_SOURCE_UNKNOWN = 3,
};

#define SLPC_POWER_PLAN_SOURCE(plan, source) ((plan) | ((source) << 6))
#define SLPC_POWER_PLAN(plan_source) ((plan_source) & 0x3F)
#define SLPC_POWER_SOURCE(plan_source) ((plan_source) >> 6)

struct slpc_platform_info {
	u8 platform_sku;
	u8 slice_count;
	u8 reserved;
	u8 power_plan_source;
	u8 P0_freq;
	u8 P1_freq;
	u8 Pe_freq;
	u8 Pn_freq;
	u32 reserved1;
	u32 reserved2;
} __packed;

struct slpc_task_state_data {
	union {
		u32 bitfield1;
		struct {
			u32 gtperf_task_active:1;
			u32 gtperf_stall_possible:1;
			u32 gtperf_gaming_mode:1;
			u32 gtperf_target_fps:8;
			u32 dcc_task_active:1;
			u32 in_dcc:1;
			u32 in_dct:1;
			u32 freq_switch_active:1;
			u32 ibc_enabled:1;
			u32 ibc_active:1;
			u32 pg1_enabled:1;
			u32 pg1_active:1;
			u32 reserved:13;
		};
	};
	union {
		u32 bitfield2;
		struct {
			u32 freq_unslice_max:8;
			u32 freq_unslice_min:8;
			u32 freq_slice_max:8;
			u32 freq_slice_min:8;
		};
	};
};

#define SLPC_MAX_OVERRIDE_PARAMETERS 192
#define SLPC_OVERRIDE_BITFIELD_SIZE ((SLPC_MAX_OVERRIDE_PARAMETERS + 31) / 32)

struct slpc_shared_data {
	u32 reserved;
	u32 shared_data_size;
	u32 global_state;
	struct slpc_platform_info platform_info;
	struct slpc_task_state_data task_state_data;
	u32 override_parameters_set_bits[SLPC_OVERRIDE_BITFIELD_SIZE];
	u32 override_parameters_values[SLPC_MAX_OVERRIDE_PARAMETERS];
} __packed;

struct intel_slpc {
	struct i915_vma *vma;
	bool enabled;
};

/* intel_slpc.c */
void intel_slpc_init(struct drm_i915_private *dev_priv);
void intel_slpc_cleanup(struct drm_i915_private *dev_priv);
void intel_slpc_suspend(struct drm_i915_private *dev_priv);
void intel_slpc_disable(struct drm_i915_private *dev_priv);
void intel_slpc_enable(struct drm_i915_private *dev_priv);
void intel_slpc_unset_param(struct drm_i915_private *dev_priv,
			    enum slpc_param_id id);
void intel_slpc_set_param(struct drm_i915_private *dev_priv,
			  enum slpc_param_id id,
			  u32 value);
void intel_slpc_get_param(struct drm_i915_private *dev_priv,
			  enum slpc_param_id id,
			  int *overriding, u32 *value);
#endif
