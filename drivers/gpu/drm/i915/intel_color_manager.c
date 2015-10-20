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

static int chv_set_degamma(struct drm_device *dev,
	struct drm_property_blob *blob, struct drm_crtc *crtc)
{
	u16 red_fract, green_fract, blue_fract;
	u32 red, green, blue;
	u32 num_samples;
	u32 word = 0;
	u32 count, cgm_control_reg, cgm_degamma_reg;
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
		cgm_control_reg = I915_READ(_PIPE_CGM_CONTROL(pipe));
		cgm_control_reg &= ~CGM_DEGAMMA_EN;
		state->palette_before_ctm_blob = NULL;

		I915_WRITE(_PIPE_CGM_CONTROL(pipe), cgm_control_reg);
		DRM_DEBUG_DRIVER("DeGamma disabled on Pipe %c\n",
				pipe_name(pipe));
		break;

	case CHV_DEGAMMA_MAX_VALS:
		cgm_degamma_reg = _PIPE_DEGAMMA_BASE(pipe);
		count = 0;
		correction_values = (struct drm_r32g32b32 *)&degamma_data->lut;
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

			blue_fract = GET_BITS(blue, 8, 14);
			green_fract = GET_BITS(green, 8, 14);
			red_fract = GET_BITS(red, 8, 14);

			/* Green (29:16) and Blue (13:0) in DWORD1 */
			SET_BITS(word, green_fract, 16, 14);
			SET_BITS(word, blue_fract, 0, 14);
			I915_WRITE(cgm_degamma_reg, word);
			cgm_degamma_reg += 4;

			/* Red (13:0) to be written to DWORD2 */
			word = red_fract;
			I915_WRITE(cgm_degamma_reg, word);
			cgm_degamma_reg += 4;
			count++;
		}

		DRM_DEBUG_DRIVER("DeGamma LUT loaded for Pipe %c\n",
				pipe_name(pipe));

		/* Enable DeGamma on Pipe */
		I915_WRITE(_PIPE_CGM_CONTROL(pipe),
			I915_READ(_PIPE_CGM_CONTROL(pipe)) | CGM_DEGAMMA_EN);

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
	u32 count, cgm_gamma_reg, cgm_control_reg;
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
		cgm_control_reg = I915_READ(_PIPE_CGM_CONTROL(pipe));
		cgm_control_reg &= ~CGM_GAMMA_EN;
		I915_WRITE(_PIPE_CGM_CONTROL(pipe), cgm_control_reg);
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
			I915_WRITE(cgm_gamma_reg, word);
			cgm_gamma_reg += 4;

			/* Red (9:0) to be written */
			word = red_fract;
			I915_WRITE(cgm_gamma_reg, word);

			cgm_gamma_reg += 4;
			count++;
		}

		/* Enable (CGM) Gamma on Pipe */
		I915_WRITE(_PIPE_CGM_CONTROL(pipe),
		I915_READ(_PIPE_CGM_CONTROL(pipe)) | CGM_GAMMA_EN);
		DRM_DEBUG_DRIVER("CGM Gamma enabled on Pipe %c\n",
			pipe_name(pipe));
		return 0;

	default:
		DRM_ERROR("Invalid number of samples (%u) for Gamma LUT\n",
				num_samples);
		return -EINVAL;
	}
}

void intel_attach_color_properties_to_crtc(struct drm_device *dev,
		struct drm_crtc *crtc)
{
	struct drm_mode_config *config = &dev->mode_config;
	struct drm_mode_object *mode_obj = &crtc->base;

	/*
	* Register:
	* =========
	* Gamma correction as palette_after_ctm property
	* Degamma correction as palette_before_ctm property
	*
	* Load:
	* =====
	* no. of coefficients supported on this platform for gamma
	* and degamma with the query properties. A user
	* space agent should read these query property, and prepare
	* the color correction values accordingly. Its expected from the
	* driver to load the right number of coefficients during the init
	* phase.
	*/
	if (config->cm_coeff_after_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_coeff_after_ctm_property,
		INTEL_INFO(dev)->num_samples_after_ctm);
		DRM_DEBUG_DRIVER("Gamma query property initialized\n");
	}

	if (config->cm_coeff_before_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_coeff_before_ctm_property,
		INTEL_INFO(dev)->num_samples_before_ctm);
		DRM_DEBUG_DRIVER("Degamma query property initialized\n");
	}

	/* Gamma correction */
	if (config->cm_palette_after_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_palette_after_ctm_property, 0);
		DRM_DEBUG_DRIVER("gamma property attached to CRTC\n");
	}

	/* Degamma correction */
	if (config->cm_palette_before_ctm_property) {
		drm_object_attach_property(mode_obj,
			config->cm_palette_before_ctm_property, 0);
		DRM_DEBUG_DRIVER("degamma property attached to CRTC\n");
	}
}
