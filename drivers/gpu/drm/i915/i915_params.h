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

#define I915_PARAMS_FOR_EACH(func) \
	func(int, modeset, -1) \
	func(int, panel_ignore_lid, 1) \
	func(int, semaphores, -1) \
	func(int, lvds_channel_mode, 0) \
	func(int, panel_use_ssc, -1) \
	func(int, vbt_sdvo_panel_type, -1) \
	func(int, enable_rc6, -1) \
	func(int, enable_dc, -1) \
	func(int, enable_fbc, -1) \
	func(int, enable_ppgtt, -1) \
	func(int, enable_execlists, -1) \
	func(int, enable_psr, -1) \
	func(int, disable_power_well, -1) \
	func(int, enable_ips, 1) \
	func(int, invert_brightness, 0) \
	func(int, enable_guc_loading, 0) \
	func(int, enable_guc_submission, 0) \
	func(int, guc_log_level, -1) \
	func(char *, guc_firmware_path, NULL) \
	func(char *, huc_firmware_path, NULL) \
	func(int, use_mmio_flip, 0) \
	func(int, mmio_debug, 0) \
	func(int, edp_vswing, 0) \
	func(unsigned int, inject_load_failure, 0) \
	/* leave bools at the end to not create holes */ \
	func(bool, alpha_support, IS_ENABLED(CONFIG_DRM_I915_ALPHA_SUPPORT)) \
	func(bool, enable_cmd_parser, true) \
	func(bool, enable_hangcheck, true) \
	func(bool, fastboot, false) \
	func(bool, prefault_disable, false) \
	func(bool, load_detect_test, false) \
	func(bool, force_reset_modeset_test, false) \
	func(bool, reset, true) \
	func(bool, error_capture, true) \
	func(bool, disable_display, false) \
	func(bool, verbose_state_checks, true) \
	func(bool, nuclear_pageflip, false) \
	func(bool, enable_dp_mst, true) \
	func(bool, enable_dpcd_backlight, false) \
	func(bool, enable_gvt, false)

#define MEMBER(T, member, value) T member;
struct i915_params {
	I915_PARAMS_FOR_EACH(MEMBER)
};
#undef MEMBER

extern struct i915_params i915 __read_mostly;

#endif

