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
 * Authors:
 * Shashank Sharma <shashank.sharma@intel.com>
 * Kausal Malladi <Kausal.Malladi@intel.com>
 */

#include "intel_color_manager.h"

static u16 chv_prepare_csc_coeff(s64 csc_coeff)
{
	u16 csc_s3_12_format = 0;
	u16 csc_int_value;
	u16 csc_fract_value;

	if (csc_coeff < 0)
		csc_s3_12_format = CSC_COEFF_SIGN;
	csc_coeff = abs(csc_coeff);
	csc_coeff += CHV_CSC_FRACT_ROUNDOFF;
	if (csc_coeff > CHV_CSC_COEFF_MAX + 1)
		csc_coeff = CHV_CSC_COEFF_MAX + 1;
	else if (csc_coeff > CHV_CSC_COEFF_MAX)
		csc_coeff = CHV_CSC_COEFF_MAX;

	csc_int_value = csc_coeff >> CHV_CSC_COEFF_SHIFT;
	csc_int_value <<= CHV_CSC_COEFF_INT_SHIFT;

	csc_fract_value = csc_coeff >> CHV_CSC_COEFF_FRACT_SHIFT;

	csc_s3_12_format |= csc_int_value | csc_fract_value;

	return csc_s3_12_format;
}

static int chv_set_csc(struct drm_device *dev, struct drm_property_blob *blob,
		struct drm_crtc *crtc)
{
	u16 temp;
	u32 word = 0;
	int count = 0;
	i915_reg_t val;
	struct drm_ctm *csc_data;
	struct drm_i915_private *dev_priv = dev->dev_private;
	enum pipe pipe;

	if (WARN_ON(!blob))
		return -EINVAL;

	if (blob->length != sizeof(struct drm_ctm)) {
		DRM_ERROR("Invalid length of data received\n");
		return -EINVAL;
	}

	csc_data = (struct drm_ctm *)blob->data;
	pipe = to_intel_crtc(crtc)->pipe;

	/* Disable CSC functionality */
	val = _MMIO(_PIPE_CGM_CONTROL(pipe));
	I915_WRITE(val, I915_READ(val) & (~CGM_CSC_EN));

	DRM_DEBUG_DRIVER("Disabled CSC Functionality on Pipe %c\n",
			pipe_name(pipe));

	val = _MMIO(_PIPE_CSC_BASE(pipe));

	/*
	* First 8 of 9 CSC correction values go in pair, to first
	* 4 CSC register (bit 0:15 and 16:31)
	*/
	while (count < CSC_MAX_VALS - 1) {
		temp = chv_prepare_csc_coeff(
					csc_data->ctm_coeff[count]);
		SET_BITS(word, temp, 0, 16);
		count++;

		temp = chv_prepare_csc_coeff(
				csc_data->ctm_coeff[count]);
		SET_BITS(word, temp, 16, 16);
		count++;

		I915_WRITE(val, word);
		val.reg += 4;
	}

	/* 9th coeff goes to 5th register, bit 0:16 */
	temp = chv_prepare_csc_coeff(
			csc_data->ctm_coeff[count]);
	SET_BITS(word, temp, 0, 16);
	I915_WRITE(val, word);

	/* Enable CSC functionality */
	val = _MMIO(_PIPE_CGM_CONTROL(pipe));
	I915_WRITE(val, I915_READ(val) | CGM_CSC_EN);
	DRM_DEBUG_DRIVER("CSC enabled on Pipe %c\n", pipe_name(pipe));
	return 0;
}

static int chv_set_degamma(struct drm_device *dev,
	struct drm_property_blob *blob, struct drm_crtc *crtc)
{
	u16 red_fract, green_fract, blue_fract;
	u32 red, green, blue;
	u32 num_samples;
	u32 word = 0;
	u32 count, cgm_control, cgm_degamma_reg;
	i915_reg_t val;
	enum pipe pipe;
	struct drm_palette *degamma_data;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_r32g32b32 *correction_values = NULL;
	struct drm_crtc_state *state = crtc->state;

	if (WARN_ON(!blob))
		return -EINVAL;

	degamma_data = (struct drm_palette *)blob->data;
	pipe = to_intel_crtc(crtc)->pipe;
	num_samples = blob->length / sizeof(struct drm_r32g32b32);

	switch (num_samples) {
	case GAMMA_DISABLE_VALS:
		/* Disable DeGamma functionality on Pipe - CGM Block */
		val = _MMIO(_PIPE_CGM_CONTROL(pipe));
		cgm_control = I915_READ(val);
		cgm_control &= ~CGM_DEGAMMA_EN;
		state->palette_before_ctm_blob = NULL;

		I915_WRITE(val, cgm_control);
		DRM_DEBUG_DRIVER("DeGamma disabled on Pipe %c\n",
				pipe_name(pipe));
		break;

	case CHV_DEGAMMA_MAX_VALS:
		cgm_degamma_reg = _PIPE_DEGAMMA_BASE(pipe);
		count = 0;
		correction_values = degamma_data->lut;
		while (count < CHV_DEGAMMA_MAX_VALS) {
			blue = correction_values[count].b32;
			green = correction_values[count].g32;
			red = correction_values[count].r32;

			if (blue > CHV_MAX_GAMMA)
				blue = CHV_MAX_GAMMA;

			if (green > CHV_MAX_GAMMA)
				green = CHV_MAX_GAMMA;

			if (red > CHV_MAX_GAMMA)
				red = CHV_MAX_GAMMA;

			blue_fract = GET_BITS(blue, 10, 14);
			green_fract = GET_BITS(green, 10, 14);
			red_fract = GET_BITS(red, 10, 14);

			/* Green (29:16) and Blue (13:0) in DWORD1 */
			SET_BITS(word, green_fract, 16, 14);
			SET_BITS(word, blue_fract, 0, 14);
			val = _MMIO(cgm_degamma_reg);
			I915_WRITE(val, word);
			cgm_degamma_reg += 4;

			/* Red (13:0) to be written to DWORD2 */
			word = red_fract;
			val = _MMIO(cgm_degamma_reg);
			I915_WRITE(val, word);
			cgm_degamma_reg += 4;
			count++;
		}

		DRM_DEBUG_DRIVER("DeGamma LUT loaded for Pipe %c\n",
				pipe_name(pipe));

		/* Enable DeGamma on Pipe */
		val = _MMIO(_PIPE_CGM_CONTROL(pipe));
		I915_WRITE(val, I915_READ(val) | CGM_DEGAMMA_EN);
		DRM_DEBUG_DRIVER("DeGamma correction enabled on Pipe %c\n",
				pipe_name(pipe));
		break;

	default:
		DRM_ERROR("Invalid number of samples for DeGamma LUT\n");
		return -EINVAL;
	}
	return 0;
}

static int chv_set_gamma(struct drm_device *dev, struct drm_property_blob *blob,
		struct drm_crtc *crtc)
{
	enum pipe pipe;
	u16 red_fract, green_fract, blue_fract;
	u32 red, green, blue, num_samples;
	u32 word = 0;
	u32 count, cgm_gamma_reg, cgm_control;
	i915_reg_t val;
	struct drm_r32g32b32 *correction_values;
	struct drm_palette *gamma_data;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_crtc_state *state = crtc->state;

	if (WARN_ON(!blob))
		return -EINVAL;

	gamma_data = (struct drm_palette *)blob->data;
	pipe = to_intel_crtc(crtc)->pipe;
	num_samples = blob->length / sizeof(struct drm_r32g32b32);

	switch (num_samples) {
	case GAMMA_DISABLE_VALS:

		/* Disable Gamma functionality on Pipe - CGM Block */
		val = _MMIO(_PIPE_CGM_CONTROL(pipe));
		cgm_control = I915_READ(val);
		cgm_control &= ~CGM_GAMMA_EN;
		I915_WRITE(val, cgm_control);
		state->palette_after_ctm_blob = NULL;
		DRM_DEBUG_DRIVER("Gamma disabled on Pipe %c\n",
			pipe_name(pipe));
		return 0;

	case CHV_8BIT_GAMMA_MAX_VALS:
	case CHV_10BIT_GAMMA_MAX_VALS:

		count = 0;
		cgm_gamma_reg = _PIPE_GAMMA_BASE(pipe);
		correction_values = gamma_data->lut;

		while (count < num_samples) {
			blue = correction_values[count].b32;
			green = correction_values[count].g32;
			red = correction_values[count].r32;

			if (blue > CHV_MAX_GAMMA)
				blue = CHV_MAX_GAMMA;

			if (green > CHV_MAX_GAMMA)
				green = CHV_MAX_GAMMA;

			if (red > CHV_MAX_GAMMA)
				red = CHV_MAX_GAMMA;

			/* get MSB 10 bits from fraction part (14:23) */
			blue_fract = GET_BITS(blue, 14, 10);
			green_fract = GET_BITS(green, 14, 10);
			red_fract = GET_BITS(red, 14, 10);

			/* Green (25:16) and Blue (9:0) to be written */
			SET_BITS(word, green_fract, 16, 10);
			SET_BITS(word, blue_fract, 0, 10);
			val = _MMIO(cgm_gamma_reg);
			I915_WRITE(val, word);
			cgm_gamma_reg += 4;

			/* Red (9:0) to be written */
			word = red_fract;
			val = _MMIO(cgm_gamma_reg);
			I915_WRITE(val, word);
			cgm_gamma_reg += 4;
			count++;
		}

		/* Enable (CGM) Gamma on Pipe */
		val = _MMIO(_PIPE_CGM_CONTROL(pipe));
		I915_WRITE(val, I915_READ(val) | CGM_GAMMA_EN);
		DRM_DEBUG_DRIVER("CGM Gamma enabled on Pipe %c\n",
			pipe_name(pipe));
		return 0;

	default:
		DRM_ERROR("Invalid number of samples (%u) for Gamma LUT\n",
				num_samples);
		return -EINVAL;
	}
}

void intel_color_manager_commit(struct drm_device *dev,
				struct drm_crtc_state *crtc_state)
{
	struct drm_property_blob *blob;
	struct drm_crtc *crtc = crtc_state->crtc;
	int ret = -EINVAL;

	/*
	* CRTC level color correction, once applied on the
	* pipe, goes on forever, until disabled, so there is no
	* need to program all those correction registers on every
	* commit. Do this only when a new correction applied.
	*/
	if (!crtc_state->color_correction_changed)
		return;

	blob = crtc_state->palette_after_ctm_blob;
	if (blob) {
		/* Gamma correction is platform specific */
		if (IS_CHERRYVIEW(dev))
			ret = chv_set_gamma(dev, blob, crtc);

		if (ret)
			DRM_ERROR("set Gamma correction failed\n");
		else
			DRM_DEBUG_DRIVER("Gamma correction success\n");
	}

	blob = crtc_state->palette_before_ctm_blob;
	if (blob) {
		/* Degamma correction */
		if (IS_CHERRYVIEW(dev))
			ret = chv_set_degamma(dev, blob, crtc);

		if (ret)
			DRM_ERROR("set degamma correction failed\n");
		else
			DRM_DEBUG_DRIVER("degamma correction success\n");
	}

	blob = crtc_state->ctm_blob;
	if (blob) {
		/* CSC correction */
		if (IS_CHERRYVIEW(dev))
			ret = chv_set_csc(dev, blob, crtc);

		if (ret)
			DRM_ERROR("set CSC correction failed\n");
		else
			DRM_DEBUG_DRIVER("CSC correction success\n");
	}

	crtc_state->color_correction_changed = false;
}

void intel_crtc_attach_color_properties(struct drm_crtc *crtc)
{
}
