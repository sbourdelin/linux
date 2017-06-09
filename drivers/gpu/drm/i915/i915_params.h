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

/*
 * Each i915 params is defined as single 'func' entry:
 *	func(Type, Name, Value, Mode, Unsafe, Brief, Detailed)
 * where:
 *	@Type: int|uint|bool|charp
 *	@Name: the name of param
 *	@Value: the default value of the param
 *	@Mode: the access mode (usually 0600|0400)
 *	@Unsafe: must be either empty or _unsafe
 *	@Brief: short description text of the param
 *	@Detailed: more detailed description of the param (may be empty)
 */
#define _I915_PARAMS_FOR_EACH_BASE(func) \
	func(int, modeset, -1, 0400, , \
	     "Use kernel modesetting (KMS).", \
	     "(0=disable, 1=on, -1=force vga console preference)") \
	func(int, panel_ignore_lid, 1, 0600, _unsafe, \
	     "Override lid status.", \
	     "(0=autodetect, 1=autodetect disabled, -1=force lid closed, -2=force lid open)") \
	func(int, semaphores, -1, 0400, _unsafe, \
	     "Use semaphores for inter-ring sync.", \
	     "(-1=use per-chip defaults)") \
	func(int, lvds_channel_mode, 0, 0400, _unsafe, \
	     "Specify LVDS channel mode.", \
	     "(0=probe BIOS, 1=single-channel, 2=dual-channel)") \
	func(int, panel_use_ssc, -1, 0600, _unsafe, \
	     "Use Spread Spectrum Clock with panels [LVDS/eDP].", \
	     "(-1=auto from VBT)") \
	func(int, vbt_sdvo_panel_type, -1, 0400, _unsafe, \
	     "Override/Ignore selection of SDVO panel mode in the VBT.", \
	     "(-2=ignore, -1=auto, 0..n=index in VBT BIOS table)") \
	func(int, enable_rc6, -1, 0400, _unsafe, \
	     "Enable power-saving render C-state 6.", \
	     "(-1=use per-chip default; 0 = disable; 1 = enable rc6; 2 = enable deep rc6; 4 = enable deepest rc6)" \
	     "Different stages can be selected via bitmask values. " \
	     "For example, 3 would enable rc6 and deep rc6, and 7 would enable everything. ") \
	func(int, enable_dc, -1, 0400, _unsafe, \
	     "Enable power-saving display C-states.", \
	     "(-1=auto; 0=disable; 1=up to DC5; 2=up to DC6)") \
	func(int, enable_fbc, -1, 0600, _unsafe, \
	     "Enable frame buffer compression for power savings.", \
	     "(-1=use per-chip default)") \
	func(int, enable_ppgtt, -1, 0400, _unsafe, \
	     "Override PPGTT usage.", \
	     "(-1=auto, 0=disabled, 1=aliasing, 2=full, 3=full with extended address space)") \
	func(int, enable_execlists, -1, 0400, _unsafe, \
	     "Override execlists usage.", \
	     "(-1=auto, 0=disabled, 1=enabled)") \
	func(int, enable_psr, -1, 0600, _unsafe, \
	     "Enable PSR.", \
	     "(-1=use per-chip default, 0=disabled," \
	     "1=link mode chosen per-platform, " \
	     "2=force link-standby mode, " \
	     "3=force link-off mode)") \
	func(int, disable_power_well, -1, 0400, _unsafe, \
	     "Disable display power wells when possible.", \
	     "(-1=auto, 0=power wells always on, 1=power wells disabled when possible)") \
	func(int, enable_ips, 1, 0600, _unsafe, \
	     "Enable IPS.", ) \
	func(int, invert_brightness, 0, 0600, _unsafe, \
	     "Invert backlight brightness.", \
	     "Please report PCI device ID, subsystem vendor and subsystem " \
	     "device ID to dri-devel@lists.freedesktop.org, if your machine " \
	     "needs it. It will then be included in an upcoming module version. " \
	     "(-1=force normal, 0=machine defaults, 1=force inversion)") \
	func(int, enable_guc_loading, 0, 0400, _unsafe, \
	     "Enable GuC firmware loading.", \
	     "(-1=auto, 0=never, 1=if available, 2=required)") \
	func(int, enable_guc_submission, 0, 0400, _unsafe, \
	     "Enable GuC submission.", \
	     "(-1=auto, 0=never, 1=if available, 2=required)") \
	func(int, guc_log_level, -1, 0400, _unsafe, \
	     "GuC firmware logging level.", \
	     "(-1:disabled, 0-3:enabled)") \
	func(charp, guc_firmware_path, NULL, 0400, _unsafe, \
	     "GuC firmware path to use instead of the default one.", ) \
	func(charp, huc_firmware_path, NULL, 0400, _unsafe, \
	     "HuC firmware path to use instead of the default one.", ) \
	func(int, use_mmio_flip, 0, 0600, _unsafe, \
	     "Use MMIO flips.", \
	     "(-1=never, 0=driver discretion, 1=always)") \
	func(int, mmio_debug, 0, 0600, , \
	     "Enable the MMIO debug code for the first N failures.", \
	     "This may negatively affect performance. ") \
	func(int, edp_vswing, 0, 0400, _unsafe, \
	     "Ignore/Override vswing pre-emph table selection from VBT.", \
	     "(0=use value from VBT, 1=low power swing(200mV), 2=default swing(400mV))") \
	func(uint, inject_load_failure, 0, 0400, _unsafe, \
	     "For developers only: Force an error after a number of failure check points.", \
	     "(0:disabled, N:force failure at the Nth failure check point)") \
	/* leave bools at the end to not create holes */ \
	func(bool, alpha_support, IS_ENABLED(CONFIG_DRM_I915_ALPHA_SUPPORT), 0400, _unsafe, \
	     "Enable alpha quality driver support for latest hardware.", \
	     "See also CONFIG_DRM_I915_ALPHA_SUPPORT. " ) \
	func(bool, enable_cmd_parser, true, 0400, _unsafe, \
	     "Enable command parsing.", \
	     "(true=enabled, false=disabled)") \
	func(bool, enable_hangcheck, true, 0644, _unsafe, \
	     "Periodically check GPU activity for detecting hangs.", \
	     "WARNING: Disabling this can cause system wide hangs! ") \
	func(bool, fastboot, false, 0600, , \
	     "Try to skip unnecessary mode sets at boot time.", ) \
	func(bool, prefault_disable, false, 0600, _unsafe, \
	     "For developers only: Disable page prefaulting for pread/pwrite/reloc.", ) \
	func(bool, load_detect_test, false, 0600, _unsafe, \
	     "For developers only: Force-enable the VGA load detect code for testing.", ) \
	func(bool, force_reset_modeset_test, false, 0600, _unsafe, \
	     "For developers only: Force a modeset during gpu reset for testing.", ) \
	func(bool, reset, true, 0600, _unsafe, \
	     "Attempt GPU resets.", ) \
	func(bool, disable_display, false, 0400, , \
	     "Disable display.", ) \
	func(bool, verbose_state_checks, true, 0600, , \
	     "Enable verbose logs (ie. WARN_ON()) in case of unexpected HW state conditions.", ) \
	func(bool, nuclear_pageflip, false, 0400, _unsafe, \
	     "Force enable atomic functionality on platforms that don't have full support yet.", ) \
	func(bool, enable_dp_mst, true, 0600, _unsafe, \
	     "Enable multi-stream transport (MST) for new DisplayPort sinks.", ) \
	func(bool, enable_dpcd_backlight, false, 0600, , \
	     "Enable support for DPCD backlight control.", ) \
	func(bool, enable_gvt, false, 0400, , \
	     "Enable support for Intel GVT-g graphics virtualization host support.", ) \
	/* consume \ */

#if IS_ENABLED(CONFIG_DRM_I915_CAPTURE_ERROR)
#define _I915_PARAMS_FOR_EACH_CONFIG_ERROR(func) \
	func(bool, error_capture, true, 0600, , \
	     "Record the GPU state following a hang.", \
	     "This information in /sys/class/drm/card<N>/error is vital for triaging and debugging hangs. ")
#else
#define _I915_PARAMS_FOR_EACH_CONFIG_ERROR(func)
#endif

#define I915_PARAMS_FOR_EACH(func) \
	_I915_PARAMS_FOR_EACH_BASE(func) \
	_I915_PARAMS_FOR_EACH_CONFIG_ERROR(func) \
	/* consume \ */

#define charp char*
#define MEMBER(Type, Name, Value, Mode, Unsafe, Brief, Detailed) Type Name;
struct i915_params {
	I915_PARAMS_FOR_EACH(MEMBER)
};
#undef MEMBER
#undef charp

extern struct i915_params i915 __read_mostly;

#endif
