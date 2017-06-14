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
 * Each i915 module param is defined as single 'param' entry:
 *	param(Name, Type, Value, Unsafe, Mode, Brief, Detailed)
 * where:
 *	@Name: the name of param
 *	@Type: int|uint|bool|charp
 *	@Value: the default value of the param
 *	@Unsafe: must be either empty or _unsafe
 *	@Mode: the access mode (usually 0600|0400)
 *	@Brief: short description text of the param
 *	@Detailed: more detailed description of the param (may be empty)
 */
#define _I915_PARAMS_FOR_EACH_BASE(param) \
	param(modeset, int, -1, /*safe*/, 0400, \
	      "Use kernel modesetting (KMS).", \
	      "(0=disable, 1=on, -1=force vga console preference)") \
	param(panel_ignore_lid, int, 1, _unsafe, 0600, \
	      "Override lid status.", \
	      "(0=autodetect, 1=autodetect disabled, -1=force lid closed, -2=force lid open)") \
	param(semaphores, int, -1, _unsafe, 0400, \
	      "Use semaphores for inter-ring sync.", \
	      "(-1=use per-chip defaults)") \
	param(lvds_channel_mode, int, 0, _unsafe, 0400, \
	      "Specify LVDS channel mode.", \
	      "(0=probe BIOS, 1=single-channel, 2=dual-channel)") \
	param(panel_use_ssc, int, -1, _unsafe, 0600, \
	      "Use Spread Spectrum Clock with panels [LVDS/eDP].", \
	      "(-1=auto from VBT)") \
	param(vbt_sdvo_panel_type, int, -1, _unsafe, 0400, \
	      "Override/Ignore selection of SDVO panel mode in the VBT.", \
	      "(-2=ignore, -1=auto, 0..n=index in VBT BIOS table)") \
	param(enable_rc6, int, -1, _unsafe, 0400, \
	      "Enable power-saving render C-state 6.", \
	      "(-1=use per-chip default, 0=disable, 1=enable rc6, 2=enable deep rc6, 4=enable deepest rc6)" \
	      "Different stages can be selected via bitmask values. " \
	      "For example, 3 would enable rc6 and deep rc6, and 7 would enable everything. ") \
	param(enable_dc, int, -1, _unsafe, 0400, \
	      "Enable power-saving display C-states.", \
	      "(-1=auto, 0=disable, 1=up to DC5, 2=up to DC6)") \
	param(enable_fbc, int, -1, _unsafe, 0600, \
	      "Enable frame buffer compression for power savings.", \
	      "(-1=use per-chip default)") \
	param(enable_ppgtt, int, -1, _unsafe, 0400, \
	      "Override PPGTT usage.", \
	      "(-1=auto, 0=disabled, 1=aliasing, 2=full, 3=full with extended address space)") \
	param(enable_execlists, int, -1, _unsafe, 0400, \
	      "Override execlists usage.", \
	      "(-1=auto, 0=disabled, 1=enabled)") \
	param(enable_psr, int, -1, _unsafe, 0600, \
	      "Enable PSR.", \
	      "(-1=use per-chip default, " \
	      "0=disabled, 1=link mode chosen per-platform, " \
	      "2=force link-standby mode, 3=force link-off mode)") \
	param(disable_power_well, int, -1, _unsafe, 0400, \
	      "Disable display power wells when possible.", \
	      "(-1=auto, 0=power wells always on, 1=power wells disabled when possible)") \
	param(enable_ips, int, 1, _unsafe, 0600, \
	      "Enable IPS.", ) \
	param(invert_brightness, int, 0, _unsafe, 0600, \
	      "Invert backlight brightness.", \
	      "Please report PCI device ID, subsystem vendor and subsystem " \
	      "device ID to dri-devel@lists.freedesktop.org, if your machine " \
	      "needs it. It will then be included in an upcoming module version. " \
	      "(-1=force normal, 0=machine defaults, 1=force inversion)") \
	param(enable_guc_loading, int, 0, _unsafe, 0400, \
	      "Enable GuC firmware loading.", \
	      "(-1=auto, 0=never, 1=if available, 2=required)") \
	param(enable_guc_submission, int, 0, _unsafe, 0400, \
	      "Enable GuC submission.", \
	      "(-1=auto, 0=never, 1=if available, 2=required)") \
	param(guc_log_level, int, -1, _unsafe, 0400, \
	      "GuC firmware logging level.", \
	      "(-1:disabled, 0-3:enabled)") \
	param(guc_firmware_path, charp, NULL, _unsafe, 0400, \
	      "GuC firmware path to use instead of the default one.", ) \
	param(huc_firmware_path, charp, NULL, _unsafe, 0400, \
	      "HuC firmware path to use instead of the default one.", ) \
	param(use_mmio_flip, int, 0, _unsafe, 0600, \
	      "Use MMIO flips.", \
	      "(-1=never, 0=driver discretion, 1=always)") \
	param(mmio_debug, int, 0, /*safe*/, 0600, \
	      "Enable the MMIO debug code for the first N failures.", \
	      "This may negatively affect performance. ") \
	param(edp_vswing, int, 0, _unsafe, 0400, \
	      "Ignore/Override vswing pre-emph table selection from VBT.", \
	      "(0=use value from VBT, 1=low power swing(200mV), 2=default swing(400mV))") \
	param(inject_load_failure, uint, 0, _unsafe, 0400, \
	      "For developers only: Force an error after a number of failure check points.", \
	      "(0:disabled, N:force failure at the Nth failure check point)") \
	/* leave bools at the end to not create holes */ \
	param(alpha_support, bool, IS_ENABLED(CONFIG_DRM_I915_ALPHA_SUPPORT), _unsafe, 0400, \
	      "Enable alpha quality driver support for latest hardware.", \
	      "See also CONFIG_DRM_I915_ALPHA_SUPPORT. " ) \
	param(enable_cmd_parser, bool, true, _unsafe, 0400, \
	      "Enable command parsing.", \
	     "(true=enabled, false=disabled)") \
	param(enable_hangcheck, bool, true, _unsafe, 0644, \
	     "Periodically check GPU activity for detecting hangs.", \
	     "WARNING: Disabling this can cause system wide hangs! ") \
	param(fastboot, bool, false, /*safe*/, 0600, \
	     "Try to skip unnecessary mode sets at boot time.", ) \
	param(prefault_disable, bool, false, _unsafe, 0600, \
	     "For developers only: Disable page prefaulting for pread/pwrite/reloc.", ) \
	param(load_detect_test, bool, false, _unsafe, 0600, \
	     "For developers only: Force-enable the VGA load detect code for testing.", ) \
	param(force_reset_modeset_test, bool, false, _unsafe, 0600, \
	     "For developers only: Force a modeset during gpu reset for testing.", ) \
	param(reset, bool, true, _unsafe, 0600, \
	     "Attempt GPU resets.", ) \
	param(disable_display, bool, false, /*safe*/, 0400, \
	     "Disable display.", ) \
	param(verbose_state_checks, bool, true, /*safe*/, 0600, \
	     "Enable verbose logs (ie. WARN_ON()) in case of unexpected HW state conditions.", ) \
	param(nuclear_pageflip, bool, false, _unsafe, 0400, \
	     "Force enable atomic functionality on platforms that don't have full support yet.", ) \
	param(enable_dp_mst, bool, true, _unsafe, 0600, \
	     "Enable multi-stream transport (MST) for new DisplayPort sinks.", ) \
	param(enable_dpcd_backlight, bool, false, /*safe*/, 0600, \
	     "Enable support for DPCD backlight control.", ) \
	param(enable_gvt, bool, false, /*safe*/, 0400, \
	     "Enable support for Intel GVT-g graphics virtualization host support.", ) \
	/* consume \ */

#if IS_ENABLED(CONFIG_DRM_I915_CAPTURE_ERROR)
#define _I915_PARAMS_FOR_EACH_CONFIG_ERROR(param) \
	param(error_capture, bool, true, /*safe*/, 0600, \
	      "Record the GPU state following a hang.", \
	      "This information in /sys/class/drm/card<N>/error is vital for triaging and debugging hangs. ")
#else
#define _I915_PARAMS_FOR_EACH_CONFIG_ERROR(param)
#endif

#define I915_PARAMS_FOR_EACH(param) \
	_I915_PARAMS_FOR_EACH_BASE(param) \
	_I915_PARAMS_FOR_EACH_CONFIG_ERROR(param) \
	/* consume \ */

#define charp char*
#define MEMBER(Name, Type, Value, Unsafe, Mode, Brief, Detailed) Type Name;
struct i915_params {
	I915_PARAMS_FOR_EACH(MEMBER)
};
#undef MEMBER
#undef charp

extern struct i915_params i915 __read_mostly;

#endif
