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

#ifndef _I915_PARAMS_H_
#define _I915_PARAMS_H_

#include <linux/cache.h> /* for __read_mostly */

typedef bool param_bool;
typedef int param_int;
typedef unsigned int param_uint;

#define I915_PARAMS_FOR_EACH(func) \
	func(param_int, modeset); \
	func(param_int, panel_ignore_lid); \
	func(param_int, semaphores); \
	func(param_int, lvds_channel_mode); \
	func(param_int, panel_use_ssc); \
	func(param_int, vbt_sdvo_panel_type); \
	func(param_int, enable_rc6); \
	func(param_int, enable_dc); \
	func(param_int, enable_fbc); \
	func(param_int, enable_ppgtt); \
	func(param_int, enable_execlists); \
	func(param_int, enable_psr); \
	func(param_uint, alpha_support); \
	func(param_int, disable_power_well); \
	func(param_int, enable_ips); \
	func(param_int, invert_brightness); \
	func(param_int, enable_guc_loading); \
	func(param_int, enable_guc_submission); \
	func(param_int, guc_log_level); \
	func(param_int, use_mmio_flip); \
	func(param_int, mmio_debug); \
	func(param_int, edp_vswing); \
	func(param_uint, inject_load_failure); \
	/* leave bools at the end to not create holes */ \
	func(param_bool, enable_cmd_parser); \
	func(param_bool, enable_hangcheck); \
	func(param_bool, fastboot); \
	func(param_bool, prefault_disable); \
	func(param_bool, load_detect_test); \
	func(param_bool, force_reset_modeset_test); \
	func(param_bool, reset); \
	func(param_bool, error_capture); \
	func(param_bool, disable_display); \
	func(param_bool, verbose_state_checks); \
	func(param_bool, nuclear_pageflip); \
	func(param_bool, enable_dp_mst); \
	func(param_bool, enable_dpcd_backlight); \
	func(param_bool, enable_gvt)

#define MEMBER(T, member) T member
struct i915_params {
	I915_PARAMS_FOR_EACH(MEMBER);
};
#undef MEMBER

extern struct i915_params i915 __read_mostly;

#endif

