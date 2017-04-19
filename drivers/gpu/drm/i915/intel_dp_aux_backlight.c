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

#include "intel_drv.h"

static void set_aux_backlight_enable(struct intel_dp *intel_dp, bool enable)
{
	uint8_t reg_val = 0;

	/* Early return when display use other mechanism to enable backlight. */
	if (!(intel_dp->edp_dpcd[1] & DP_EDP_BACKLIGHT_AUX_ENABLE_CAP))
		return;

	if (drm_dp_dpcd_readb(&intel_dp->aux, DP_EDP_DISPLAY_CONTROL_REGISTER,
			      &reg_val) < 0) {
		DRM_DEBUG_KMS("Failed to read DPCD register 0x%x\n",
			      DP_EDP_DISPLAY_CONTROL_REGISTER);
		return;
	}
	if (enable)
		reg_val |= DP_EDP_BACKLIGHT_ENABLE;
	else
		reg_val &= ~(DP_EDP_BACKLIGHT_ENABLE);

	if (drm_dp_dpcd_writeb(&intel_dp->aux, DP_EDP_DISPLAY_CONTROL_REGISTER,
			       reg_val) != 1) {
		DRM_DEBUG_KMS("Failed to %s aux backlight\n",
			      enable ? "enable" : "disable");
	}
}

/*
 * Read the current backlight value from DPCD register(s) based
 * on if 8-bit(MSB) or 16-bit(MSB and LSB) values are supported
 */
static uint32_t intel_dp_aux_get_backlight(struct intel_connector *connector)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&connector->encoder->base);
	uint8_t read_val[2] = { 0x0 };
	uint16_t level = 0;

	if (drm_dp_dpcd_read(&intel_dp->aux, DP_EDP_BACKLIGHT_BRIGHTNESS_MSB,
			     &read_val, sizeof(read_val)) < 0) {
		DRM_DEBUG_KMS("Failed to read DPCD register 0x%x\n",
			      DP_EDP_BACKLIGHT_BRIGHTNESS_MSB);
		return 0;
	}
	level = read_val[0];
	if (intel_dp->edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_BYTE_COUNT)
		level = (read_val[0] << 8 | read_val[1]);

	return level;
}

/*
 * Sends the current backlight level over the aux channel, checking if its using
 * 8-bit or 16 bit value (MSB and LSB)
 */
static void
intel_dp_aux_set_backlight(struct intel_connector *connector, u32 level)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&connector->encoder->base);
	uint8_t vals[2] = { 0x0 };

	vals[0] = level;

	/* Write the MSB and/or LSB */
	if (intel_dp->edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_BYTE_COUNT) {
		vals[0] = (level & 0xFF00) >> 8;
		vals[1] = (level & 0xFF);
	}
	if (drm_dp_dpcd_write(&intel_dp->aux, DP_EDP_BACKLIGHT_BRIGHTNESS_MSB,
			      vals, sizeof(vals)) < 0) {
		DRM_DEBUG_KMS("Failed to write aux backlight level\n");
		return;
	}
	connector->panel.backlight.level = level;
}

/*
 * Set minimum / maximum dynamic brightness percentage. This value is expressed
 * as the percentage of normal brightness in 5% increments.
 */
static void
intel_dp_aux_set_dynamic_backlight_percent(struct intel_dp *intel_dp,
					   u32 min, u32 max)
{
	u8 dbc[] = { DIV_ROUND_CLOSEST(min, 5), DIV_ROUND_CLOSEST(max, 5) };
	drm_dp_dpcd_write(&intel_dp->aux, DP_EDP_DBC_MINIMUM_BRIGHTNESS_SET,
			  dbc, sizeof(dbc));
}

/*
 * Set PWM Frequency divider to match desired frequency in vbt.
 * The PWM Frequency is calculated as 27Mhz / (F x P).
 * - Where F = PWM Frequency Pre-Divider value programmed by field 7:0 of the
 *             EDP_BACKLIGHT_FREQ_SET register (DPCD Address 00728h)
 * - Where P = 2^Pn, where Pn is the value programmed by field 4:0 of the
 *             EDP_PWMGEN_BIT_COUNT register (DPCD Address 00724h)
 */
static void intel_dp_aux_set_pwm_freq(struct intel_connector *connector)
{
	struct drm_i915_private *dev_priv = to_i915(connector->base.dev);
	struct intel_dp *intel_dp = enc_to_intel_dp(&connector->encoder->base);
	int freq, fxp, f;
	u8 pn, pn_min, pn_max;

	/* Find desired value of (F x P)
	 * Note that, if F x P is out of supported range, the maximum value or
	 * minimum value will applied automatically. So no need to check that.
	 */
	freq = dev_priv->vbt.backlight.pwm_freq_hz;
	fxp = DP_EDP_BACKLIGHT_FREQ_BASE / freq;

	/* Use lowest possible value of Pn to try to make F to be between 1 and
	 * 255 while still in the range Pn_min and Pn_max
	 */
	if (!drm_dp_dpcd_readb(&intel_dp->aux,
			       DP_EDP_PWMGEN_BIT_COUNT_CAP_MIN, &pn_min)) {
		return;
	}
	if (!drm_dp_dpcd_readb(&intel_dp->aux,
			       DP_EDP_PWMGEN_BIT_COUNT_CAP_MAX, &pn_max)) {
		return;
	}
	pn_min &= DP_EDP_PWMGEN_BIT_COUNT_MASK;
	pn_max &= DP_EDP_PWMGEN_BIT_COUNT_MASK;
	f = fxp / (1 << pn_min);
	for (pn = pn_min; pn < pn_max && f > 255; pn++)
		f /= 2;

	/* Cap F to be in the range between 1 and 255. */
	f = min(f, 255);
	f = max(f, 1);

	drm_dp_dpcd_writeb(&intel_dp->aux, DP_EDP_PWMGEN_BIT_COUNT, pn);
	drm_dp_dpcd_writeb(&intel_dp->aux, DP_EDP_BACKLIGHT_FREQ_SET, (u8) f);
}

static void intel_dp_aux_enable_backlight(struct intel_connector *connector)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&connector->encoder->base);
	uint8_t dpcd_buf = 0;
	uint8_t new_dpcd_buf = 0;
	uint8_t edp_backlight_mode = 0;
	bool freq_cap;

	set_aux_backlight_enable(intel_dp, true);

	if (!drm_dp_dpcd_readb(&intel_dp->aux,
			       DP_EDP_BACKLIGHT_MODE_SET_REGISTER, &dpcd_buf)) {
		return;
	}

	new_dpcd_buf = dpcd_buf;
	edp_backlight_mode = dpcd_buf & DP_EDP_BACKLIGHT_CONTROL_MODE_MASK;

	switch (edp_backlight_mode) {
	case DP_EDP_BACKLIGHT_CONTROL_MODE_PWM:
	case DP_EDP_BACKLIGHT_CONTROL_MODE_PRESET:
	case DP_EDP_BACKLIGHT_CONTROL_MODE_PRODUCT:
		new_dpcd_buf &= ~DP_EDP_BACKLIGHT_CONTROL_MODE_MASK;
		new_dpcd_buf |= DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD;
		break;

	/* Do nothing when it is already DPCD mode */
	case DP_EDP_BACKLIGHT_CONTROL_MODE_DPCD:
	default:
		break;
	}

	if (intel_dp->edp_dpcd[2] & DP_EDP_DYNAMIC_BACKLIGHT_CAP) {
		new_dpcd_buf |= DP_EDP_DYNAMIC_BACKLIGHT_ENABLE;
		intel_dp_aux_set_dynamic_backlight_percent(intel_dp, 0, 100);
	}

	freq_cap = intel_dp->edp_dpcd[2] & DP_EDP_BACKLIGHT_FREQ_AUX_SET_CAP;
	if (freq_cap)
		new_dpcd_buf |= DP_EDP_BACKLIGHT_FREQ_AUX_SET_ENABLE;

	if (new_dpcd_buf != dpcd_buf) {
		drm_dp_dpcd_writeb(&intel_dp->aux,
			DP_EDP_BACKLIGHT_MODE_SET_REGISTER, new_dpcd_buf);
	}

	if (freq_cap)
		intel_dp_aux_set_pwm_freq(connector);

	intel_dp_aux_set_backlight(connector, connector->panel.backlight.level);
}

static void intel_dp_aux_disable_backlight(struct intel_connector *connector)
{
	set_aux_backlight_enable(enc_to_intel_dp(&connector->encoder->base), false);
}

static int intel_dp_aux_setup_backlight(struct intel_connector *connector,
					enum pipe pipe)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&connector->encoder->base);
	struct intel_panel *panel = &connector->panel;

	intel_dp_aux_enable_backlight(connector);

	if (intel_dp->edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_BYTE_COUNT)
		panel->backlight.max = 0xFFFF;
	else
		panel->backlight.max = 0xFF;

	panel->backlight.min = 0;
	panel->backlight.level = intel_dp_aux_get_backlight(connector);

	panel->backlight.enabled = panel->backlight.level != 0;

	return 0;
}

static bool
intel_dp_aux_display_control_capable(struct intel_connector *connector)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&connector->encoder->base);
	bool supported;

	/* Check the  eDP Display control capabilities registers to determine if
	 * the panel can support backlight control over the aux channel
	 */
	switch (i915.enable_dpcd_backlight) {
	case 1: /* prefer PWM pin */
		supported = (intel_dp->edp_dpcd[1] & DP_EDP_TCON_BACKLIGHT_ADJUSTMENT_CAP) &&
			    (intel_dp->edp_dpcd[1] & DP_EDP_BACKLIGHT_AUX_ENABLE_CAP) &&
			    !(intel_dp->edp_dpcd[1] & DP_EDP_BACKLIGHT_PIN_ENABLE_CAP) &&
			    !(intel_dp->edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_PWM_PIN_CAP);
		break;
	case 2: /* prefer DPCD */
		supported = (intel_dp->edp_dpcd[1] & DP_EDP_TCON_BACKLIGHT_ADJUSTMENT_CAP) &&
			    (intel_dp->edp_dpcd[2] & DP_EDP_BACKLIGHT_BRIGHTNESS_AUX_SET_CAP);
		break;
	default:
		supported = false;
	}

	if (supported)
		DRM_DEBUG_KMS("AUX Backlight Control Supported!\n");

	return supported;
}

int intel_dp_aux_init_backlight_funcs(struct intel_connector *intel_connector)
{
	struct intel_panel *panel = &intel_connector->panel;

	if (!intel_dp_aux_display_control_capable(intel_connector))
		return -ENODEV;

	panel->backlight.setup = intel_dp_aux_setup_backlight;
	panel->backlight.enable = intel_dp_aux_enable_backlight;
	panel->backlight.disable = intel_dp_aux_disable_backlight;
	panel->backlight.set = intel_dp_aux_set_backlight;
	panel->backlight.get = intel_dp_aux_get_backlight;

	return 0;
}
