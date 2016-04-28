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

#define SLPC_MAJOR_VER 2
#define SLPC_MINOR_VER 4
#define SLPC_VERSION ((2015 << 16) | (SLPC_MAJOR_VER << 8) | (SLPC_MINOR_VER))

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

enum slpc_global_state {
	SLPC_GLOBAL_STATE_NOT_RUNNING = 0,
	SLPC_GLOBAL_STATE_INITIALIZING = 1,
	SLPC_GLOBAL_STATE_RESETTING = 2,
	SLPC_GLOBAL_STATE_RUNNING = 3,
	SLPC_GLOBAL_STATE_SHUTTING_DOWN = 4,
	SLPC_GLOBAL_STATE_ERROR = 5
};

enum slpc_host_os {
	SLPC_HOST_OS_UNDEFINED = 0,
	SLPC_HOST_OS_WINDOWS_8 = 1,
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

struct slpc_platform_info {
	u8 platform_sku;
	u8 slice_count;
	u8 host_os;
	u8 power_plan_source;
	u8 P0_freq;
	u8 P1_freq;
	u8 Pe_freq;
	u8 Pn_freq;
	u32 package_rapl_limit_high;
	u32 package_rapl_limit_low;
} __packed;

#define SLPC_MAX_OVERRIDE_PARAMETERS 192
#define SLPC_OVERRIDE_BITFIELD_SIZE ((SLPC_MAX_OVERRIDE_PARAMETERS + 31) / 32)

struct slpc_shared_data {
	u32 slpc_version;
	u32 shared_data_size;
	u32 global_state;
	struct slpc_platform_info platform_info;
	u32 task_state_data;
	u32 override_parameters_set_bits[SLPC_OVERRIDE_BITFIELD_SIZE];
	u32 override_parameters_values[SLPC_MAX_OVERRIDE_PARAMETERS];
} __packed;

#define SLPC_MAX_NUM_OF_PIPES 4

struct intel_display_pipe_info {
	union {
		u32 data;
		struct {
			u32 is_widi:1;
			u32 refresh_rate:7;
			u32 vsync_ft_usec:24;
		};
	};
} __packed;

struct intel_slpc_display_mode_event_params {
	struct {
		struct intel_display_pipe_info
					per_pipe_info[SLPC_MAX_NUM_OF_PIPES];
		union {
			u32 global_data;
			struct {
				u32 active_pipes_bitmask:SLPC_MAX_NUM_OF_PIPES;
				u32 fullscreen_pipes:SLPC_MAX_NUM_OF_PIPES;
				u32 vbi_sync_on_pipes:SLPC_MAX_NUM_OF_PIPES;
				u32 num_active_pipes:2;
			};
		};
	};
} __packed;

struct intel_slpc {
	struct drm_i915_gem_object *shared_data_obj;
	struct intel_slpc_display_mode_event_params display_mode_params;
};

/* intel_slpc.c */
void intel_slpc_init(struct drm_device *dev);
void intel_slpc_cleanup(struct drm_device *dev);
void intel_slpc_suspend(struct drm_device *dev);
void intel_slpc_disable(struct drm_device *dev);
void intel_slpc_enable(struct drm_device *dev);
void intel_slpc_reset(struct drm_device *dev);
void intel_slpc_update_display_mode_info(struct drm_device *dev);
void intel_slpc_update_atomic_commit_info(struct drm_device *dev,
					  struct drm_atomic_state *state);

#endif
