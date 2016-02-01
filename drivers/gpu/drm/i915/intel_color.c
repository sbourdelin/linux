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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "intel_drv.h"

#define CTM_COEFF_SIGN	(1ULL << 63)

#define CTM_COEFF_1_0	(1ULL << 32)
#define CTM_COEFF_2_0	(CTM_COEFF_1_0 << 1)
#define CTM_COEFF_4_0	(CTM_COEFF_2_0 << 1)
#define CTM_COEFF_0_5	(CTM_COEFF_1_0 >> 1)
#define CTM_COEFF_0_25	(CTM_COEFF_0_5 >> 1)
#define CTM_COEFF_0_125	(CTM_COEFF_0_25 >> 1)

#define CTM_COEFF_LIMITED_RANGE ((235ULL - 16ULL) * CTM_COEFF_1_0 / 255)

#define CTM_COEFF_NEGATIVE(coeff)	(((coeff) & CTM_COEFF_SIGN) != 0)
#define CTM_COEFF_ABS(coeff)		((coeff) & (CTM_COEFF_SIGN - 1))

/*
 * Extract the CSC coefficient from a CTM coefficient (in U32.32 fixed point
 * format). This macro takes the coefficient we want transformed and the
 * number of fractional bits.
 *
 * We only have a 9 bits precision window which slides depending on the value
 * of the CTM coefficient and we write the value from bit 3. We also round the
 * value.
 */
#define I9XX_CSC_COEFF_FP(coeff, fbits)	\
	(clamp_val(((coeff) >> (32 - (fbits) - 3)) + 4, 0, 0xfff) & 0xff8)

#define I9XX_CSC_COEFF_LIMITED_RANGE	\
	I9XX_CSC_COEFF_FP(CTM_COEFF_LIMITED_RANGE, 9)
#define I9XX_CSC_COEFF_1_0		\
	((7 << 12) | I9XX_CSC_COEFF_FP(CTM_COEFF_1_0, 8))

/*
 * When using limited range, multiply the matrix given by userspace by
 * the matrix that we would use for the limited range. We do the
 * multiplication in U2.30 format.
 */
static void ctm_matrix_mult_by_limited(uint64_t *result,
				       int64_t *input)
{
	int i, j;
	uint64_t limited_coeffs[9] = { CTM_COEFF_LIMITED_RANGE, 0, 0,
				       0, CTM_COEFF_LIMITED_RANGE, 0,
				       0, 0, CTM_COEFF_LIMITED_RANGE };

	for (i = 0; i < ARRAY_SIZE(limited_coeffs); i++) {
		int column = i % 3, row = i / 3;
		int negative = 0;

		input[i] = 0;
		for (j = 0; j < 3; j++) {
			int64_t user_coeff = input[j * 3 + column];
			uint64_t limited_coeff =
				limited_coeffs[row * 3 + j] >> 2;
			uint64_t abs_coeff =
				clamp_val(CTM_COEFF_ABS(user_coeff),
					  0,
					  CTM_COEFF_4_0 - 1) >> 2;

			if (CTM_COEFF_NEGATIVE(user_coeff))
				negative = !negative;
			result[i] += limited_coeff * abs_coeff;
		}

		result[i] >>= 27;
		if (negative)
			result[i] |= CTM_COEFF_SIGN;
	}
}

/*
 * Set up the pipe CSC unit.
 *
 * Currently only full range RGB to limited range RGB conversion is supported,
 * but eventually this should handle various RGB<->YCbCr scenarios as well.
 */
static void i9xx_load_csc_matrix(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_crtc_state *crtc_state = crtc->state;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int i, pipe = intel_crtc->pipe;
	uint16_t coeffs[9] = { 0, };

	if (crtc_state->ctm_matrix) {
		struct drm_color_ctm *ctm =
			(struct drm_color_ctm *)crtc_state->ctm_matrix->data;
		uint64_t input[9] = { 0, };

		if (intel_crtc->config->limited_color_range)
			ctm_matrix_mult_by_limited(input, ctm->matrix);
		else {
			for (i = 0; i < ARRAY_SIZE(input); i++)
				input[i] = ctm->matrix[i];
		}


		/*
		 * Convert fixed point S31.32 input to format supported by the
		 * hardware.
		 */
		for (i = 0; i < ARRAY_SIZE(coeffs); i++) {
			uint64_t abs_coeff = ((1ULL << 63) - 1) & input[i];

			/*
			 * Clamp input value to min/max supported by
			 * hardware.
			 */
			abs_coeff = clamp_val(abs_coeff, 0, CTM_COEFF_4_0 - 1);

			/* sign bit */
			if (CTM_COEFF_NEGATIVE(input[i]))
				coeffs[i] |= 1 << 15;

			if (abs_coeff < CTM_COEFF_0_125)
				coeffs[i] |= (3 << 12) |
					I9XX_CSC_COEFF_FP(abs_coeff, 12);
			else if (abs_coeff < CTM_COEFF_0_25)
				coeffs[i] |= (2 << 12) |
					I9XX_CSC_COEFF_FP(abs_coeff, 11);
			else if (abs_coeff < CTM_COEFF_0_5)
				coeffs[i] |= (1 << 12) |
					I9XX_CSC_COEFF_FP(abs_coeff, 10);
			else if (abs_coeff < CTM_COEFF_1_0)
				coeffs[i] |= I9XX_CSC_COEFF_FP(abs_coeff, 9);
			else if (abs_coeff < CTM_COEFF_2_0)
				coeffs[i] |= (7 << 12) |
					I9XX_CSC_COEFF_FP(abs_coeff, 8);
			else
				coeffs[i] |= (6 << 12) |
					I9XX_CSC_COEFF_FP(abs_coeff, 7);
		}
	} else {
		/*
		 * Load an identify matrix if no coefficients are provided.
		 *
		 * TODO: Check what kind of values actually come out of the
		 * pipe with these coeff/postoff values and adjust to get the
		 * best accuracy. Perhaps we even need to take the bpc value
		 * into consideration.
		 */
		for (i = 0; i < 3; i++) {
			if (intel_crtc->config->limited_color_range)
				coeffs[i * 3 + i] =
					I9XX_CSC_COEFF_LIMITED_RANGE;
			else
				coeffs[i * 3 + i] = I9XX_CSC_COEFF_1_0;
		}
	}

	/*
	 * GY/GU and RY/RU should be the other way around according
	 * to BSpec, but reality doesn't agree. Just set them up in
	 * a way that results in the correct picture.
	 */
	I915_WRITE(PIPE_CSC_COEFF_RY_GY(pipe), coeffs[0] << 16 | coeffs[1]);
	I915_WRITE(PIPE_CSC_COEFF_BY(pipe), coeffs[2] << 16);

	I915_WRITE(PIPE_CSC_COEFF_RU_GU(pipe), coeffs[3] << 16 | coeffs[4]);
	I915_WRITE(PIPE_CSC_COEFF_BU(pipe), coeffs[5] << 16);

	I915_WRITE(PIPE_CSC_COEFF_RV_GV(pipe), coeffs[6] << 16 | coeffs[7]);
	I915_WRITE(PIPE_CSC_COEFF_BV(pipe), coeffs[8] << 16);

	I915_WRITE(PIPE_CSC_PREOFF_HI(pipe), 0);
	I915_WRITE(PIPE_CSC_PREOFF_ME(pipe), 0);
	I915_WRITE(PIPE_CSC_PREOFF_LO(pipe), 0);

	if (INTEL_INFO(dev)->gen > 6) {
		uint16_t postoff = 0;

		if (intel_crtc->config->limited_color_range)
			postoff = (16 * (1 << 12) / 255) & 0x1fff;

		I915_WRITE(PIPE_CSC_POSTOFF_HI(pipe), postoff);
		I915_WRITE(PIPE_CSC_POSTOFF_ME(pipe), postoff);
		I915_WRITE(PIPE_CSC_POSTOFF_LO(pipe), postoff);

		I915_WRITE(PIPE_CSC_MODE(pipe), 0);
	} else {
		uint32_t mode = CSC_MODE_YUV_TO_RGB;

		if (intel_crtc->config->limited_color_range)
			mode |= CSC_BLACK_SCREEN_OFFSET;

		I915_WRITE(PIPE_CSC_MODE(pipe), mode);
	}
}

/*
 * Set up the pipe CSC unit on CherryView.
 */
static void cherryview_load_csc_matrix(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_crtc_state *crtc_state = crtc->state;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int pipe = intel_crtc->pipe;


	if (crtc_state->ctm_matrix) {
		struct drm_color_ctm *ctm =
			(struct drm_color_ctm *)crtc_state->ctm_matrix->data;
		uint16_t coeffs[9] = { 0, };
		int i;

		for (i = 0; i < ARRAY_SIZE(coeffs); i++) {
			uint64_t abs_coeff =
				((1ULL << 63) - 1) & ctm->matrix[i];

			abs_coeff = clamp_val(abs_coeff, 0, (1 << 15) - 1);

			/* Write coefficients in S3.12 format. */
			if (ctm->matrix[i] & (1ULL << 63))
				coeffs[i] = 1 << 15;
			coeffs[i] |= ((abs_coeff >> 32) & 7) << 12;
			coeffs[i] |= (((abs_coeff >> 19) + 1) >> 1) & 0xfff;
		}

		I915_WRITE(CGM_PIPE_CSC_COEFF01(pipe),
			   coeffs[1] << 16 | coeffs[0]);
		I915_WRITE(CGM_PIPE_CSC_COEFF23(pipe),
			   coeffs[3] << 16 | coeffs[2]);
		I915_WRITE(CGM_PIPE_CSC_COEFF45(pipe),
			   coeffs[5] << 16 | coeffs[4]);
		I915_WRITE(CGM_PIPE_CSC_COEFF67(pipe),
			   coeffs[7] << 16 | coeffs[6]);
		I915_WRITE(CGM_PIPE_CSC_COEFF8(pipe), coeffs[8]);
	}

	I915_WRITE(CGM_PIPE_MODE(pipe),
		   (crtc_state->ctm_matrix ? CGM_PIPE_MODE_CSC : 0) |
		   (crtc_state->degamma_lut ? CGM_PIPE_MODE_DEGAMMA : 0) |
		   (crtc_state->gamma_lut ? CGM_PIPE_MODE_GAMMA : 0));
}

/** Loads the legacy palette/gamma unit for the CRTC with the prepared
 * values.
 */
static void i9xx_load_legacy_gamma_lut(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_crtc_state *intel_state = to_intel_crtc_state(crtc->state);
	enum pipe pipe = to_intel_crtc(crtc)->pipe;
	int i;

	for (i = 0; i < 256; i++) {
		uint32_t word = (intel_crtc->lut_r[i] << 16) |
			(intel_crtc->lut_g[i] << 8) |
			intel_crtc->lut_b[i];
		if (HAS_GMCH_DISPLAY(dev))
			I915_WRITE(PALETTE(pipe, i), word);
		else
			I915_WRITE(LGC_PALETTE(pipe, i), word);
	}

	intel_state->gamma_mode = GAMMA_MODE_MODE_8BIT;
	I915_WRITE(GAMMA_MODE(intel_crtc->pipe), GAMMA_MODE_MODE_8BIT);
}

static void broadwell_load_degamma_lut(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_crtc_state *state = crtc->state;
	struct intel_crtc_state *intel_state = to_intel_crtc_state(state);
	enum pipe pipe = to_intel_crtc(crtc)->pipe;
	uint32_t i, lut_size = INTEL_INFO(dev)->color.degamma_lut_size;

	I915_WRITE(PREC_PAL_INDEX(pipe),
		   PAL_PREC_SPLIT_MODE | PAL_PREC_AUTO_INCREMENT);

	if (state->degamma_lut) {
		struct drm_color_lut *lut =
			(struct drm_color_lut *) state->degamma_lut->data;

		for (i = 0; i < lut_size; i++) {
			uint32_t word =
			drm_color_lut_extract(lut[i].red, 10) << 20 |
			drm_color_lut_extract(lut[i].green, 10) << 10 |
			drm_color_lut_extract(lut[i].blue, 10);

			I915_WRITE(PREC_PAL_DATA(pipe), word);
		}
	} else {
		for (i = 0; i < lut_size; i++) {
			uint32_t v = (i * ((1 << 10) - 1)) / (lut_size - 1);

			I915_WRITE(PREC_PAL_DATA(pipe),
				   (v << 20) | (v << 10) | v);
		}
	}

	intel_state->gamma_mode = GAMMA_MODE_MODE_SPLIT;
	I915_WRITE(GAMMA_MODE(pipe), GAMMA_MODE_MODE_SPLIT);
	POSTING_READ(GAMMA_MODE(pipe));

	/* Reset the index, otherwise it prevents the legacy palette to be
	 * written properly.
	 */
	I915_WRITE(PREC_PAL_INDEX(pipe), 0);
}

static void cherryview_load_degamma_lut(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_crtc_state *state = crtc->state;
	enum pipe pipe = to_intel_crtc(crtc)->pipe;

	if (state->degamma_lut) {
		struct drm_color_lut *lut =
			(struct drm_color_lut *) state->degamma_lut->data;
		uint32_t i, lut_size = INTEL_INFO(dev)->color.degamma_lut_size;
		uint32_t word0, word1;

		for (i = 0; i < lut_size; i++) {
			/* Write LUT in U0.14 format. */
			word0 =
			(drm_color_lut_extract(lut[i].green, 14) << 16) |
			drm_color_lut_extract(lut[i].blue, 14);
			word1 = drm_color_lut_extract(lut[i].red, 14);

			I915_WRITE(CGM_PIPE_DEGAMMA(pipe, i, 0), word0);
			I915_WRITE(CGM_PIPE_DEGAMMA(pipe, i, 1), word1);
		}
		/* Write the 65's entry of the LUT with the last entry given
		 * by user space to clamp values > 1.0.
		 */
		word0 =
		(drm_color_lut_extract(lut[lut_size - 1].green, 14) << 16) |
		drm_color_lut_extract(lut[lut_size - 1].blue, 14);
		word1 = drm_color_lut_extract(lut[lut_size - 1].red, 14);

		I915_WRITE(CGM_PIPE_DEGAMMA(pipe, lut_size, 0), word0);
		I915_WRITE(CGM_PIPE_DEGAMMA(pipe, lut_size, 1), word1);
	}

	I915_WRITE(CGM_PIPE_MODE(pipe),
		   (state->ctm_matrix ? CGM_PIPE_MODE_CSC : 0) |
		   (state->degamma_lut ? CGM_PIPE_MODE_DEGAMMA : 0) |
		   (state->gamma_lut ? CGM_PIPE_MODE_GAMMA : 0));
}

static void broadwell_load_gamma_lut(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_crtc_state *state = crtc->state;
	struct intel_crtc_state *intel_state = to_intel_crtc_state(state);
	enum pipe pipe = to_intel_crtc(crtc)->pipe;
	uint32_t i, lut_offset = INTEL_INFO(dev)->color.degamma_lut_size,
		lut_size = INTEL_INFO(dev)->color.gamma_lut_size;


	I915_WRITE(PREC_PAL_INDEX(pipe),
		   PAL_PREC_SPLIT_MODE | PAL_PREC_AUTO_INCREMENT | lut_offset);

	if (state->gamma_lut) {
		struct drm_color_lut *lut =
			(struct drm_color_lut *) state->gamma_lut->data;

		for (i = 0; i < lut_size; i++) {
			uint32_t word =
			(drm_color_lut_extract(lut[i].red, 10) << 20) |
			(drm_color_lut_extract(lut[i].green, 10) << 10) |
			drm_color_lut_extract(lut[i].blue, 10);

			I915_WRITE(PREC_PAL_DATA(pipe), word);
		}

		/* Program the max register to clamp values > 1.0. */
		I915_WRITE(PREC_PAL_GC_MAX(pipe, 0),
			   drm_color_lut_extract(lut[i].red, 16));
		I915_WRITE(PREC_PAL_GC_MAX(pipe, 1),
			   drm_color_lut_extract(lut[i].green, 16));
		I915_WRITE(PREC_PAL_GC_MAX(pipe, 2),
			   drm_color_lut_extract(lut[i].blue, 16));
	} else {
		for (i = 0; i < lut_size; i++) {
			uint32_t v = (i * ((1 << 10) - 1)) / (lut_size - 1);

			I915_WRITE(PREC_PAL_DATA(pipe),
				   (v << 20) | (v << 10) | v);
		}

		I915_WRITE(PREC_PAL_GC_MAX(pipe, 0), (1 << 16) - 1);
		I915_WRITE(PREC_PAL_GC_MAX(pipe, 1), (1 << 16) - 1);
		I915_WRITE(PREC_PAL_GC_MAX(pipe, 2), (1 << 16) - 1);
	}

	intel_state->gamma_mode = GAMMA_MODE_MODE_SPLIT;
	I915_WRITE(GAMMA_MODE(pipe), GAMMA_MODE_MODE_SPLIT);
	POSTING_READ(GAMMA_MODE(pipe));

	/* Reset the index, otherwise it prevents the legacy palette to be
	 * written properly.
	 */
	I915_WRITE(PREC_PAL_INDEX(pipe), 0);
}

static void cherryview_load_gamma_lut(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct drm_crtc_state *state = crtc->state;
	enum pipe pipe = to_intel_crtc(crtc)->pipe;

	if (state->gamma_lut) {
		struct drm_color_lut *lut =
			(struct drm_color_lut *) state->gamma_lut->data;
		uint32_t i, lut_size = INTEL_INFO(dev)->color.gamma_lut_size;
		uint32_t word0, word1;

		for (i = 0; i < lut_size; i++) {
			/* Write LUT in U0.10 format. */
			word0 =
			(drm_color_lut_extract(lut[i].green, 10) << 16) |
			drm_color_lut_extract(lut[i].blue, 10);
			word1 = drm_color_lut_extract(lut[i].red, 10);

			I915_WRITE(CGM_PIPE_GAMMA(pipe, i, 0), word0);
			I915_WRITE(CGM_PIPE_GAMMA(pipe, i, 1), word1);
		}
		/* Write the 257's entry of the LUT with the last entry given
		 * by user space to clamp values > 1.0.
		 */
		word0 =
		(drm_color_lut_extract(lut[lut_size - 1].green, 10) << 16) |
		drm_color_lut_extract(lut[lut_size - 1].blue, 10);
		word1 = drm_color_lut_extract(lut[lut_size - 1].red, 10);

		I915_WRITE(CGM_PIPE_GAMMA(pipe, lut_size, 0), word0);
		I915_WRITE(CGM_PIPE_GAMMA(pipe, lut_size, 1), word1);
	}

	I915_WRITE(CGM_PIPE_MODE(pipe),
		   (state->ctm_matrix ? CGM_PIPE_MODE_CSC : 0) |
		   (state->degamma_lut ? CGM_PIPE_MODE_DEGAMMA : 0) |
		   (state->gamma_lut ? CGM_PIPE_MODE_GAMMA : 0));
}

static void intel_color_load_luts_internal(struct drm_crtc *crtc,
					  bool legacy)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	struct intel_crtc_state *intel_state = to_intel_crtc_state(crtc->state);
	enum pipe pipe = to_intel_crtc(crtc)->pipe;
	bool reenable_ips = false;

	/* The clocks have to be on to load the palette. */
	if (!crtc->state->active)
		return;

	if (HAS_GMCH_DISPLAY(dev)) {
		if (intel_crtc->config->has_dsi_encoder)
			assert_dsi_pll_enabled(dev_priv);
		else
			assert_pll_enabled(dev_priv, pipe);
	}

	/* Workaround : Do not read or write the pipe palette/gamma data while
	 * GAMMA_MODE is configured for split gamma and IPS_CTL has IPS enabled.
	 */
	if (IS_HASWELL(dev) && intel_crtc->config->ips_enabled &&
	    intel_state->gamma_mode == GAMMA_MODE_MODE_SPLIT) {
		hsw_disable_ips(intel_crtc);
		reenable_ips = true;
	}

	if (legacy)
		i9xx_load_legacy_gamma_lut(crtc);
	else {
		dev_priv->display.load_degamma_lut(crtc);
		dev_priv->display.load_gamma_lut(crtc);
	}

	if (reenable_ips)
		hsw_enable_ips(intel_crtc);
}

void intel_color_legacy_load_lut(struct drm_crtc *crtc)
{
	intel_color_load_luts_internal(crtc, true);
}

void intel_color_legacy_gamma_set(struct drm_crtc *crtc, u16 *red, u16 *green,
				  u16 *blue, uint32_t start, uint32_t size)
{
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int end = (start + size > 256) ? 256 : start + size, i;

	for (i = start; i < end; i++) {
		intel_crtc->lut_r[i] = red[i] >> 8;
		intel_crtc->lut_g[i] = green[i] >> 8;
		intel_crtc->lut_b[i] = blue[i] >> 8;
	}

	intel_color_load_luts_internal(crtc, true);
}

void intel_color_load_luts(struct drm_crtc *crtc)
{
	intel_color_load_luts_internal(crtc,
				       !crtc->state->degamma_lut &&
				       !crtc->state->gamma_lut);
}

void intel_color_set_csc(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;

	if (dev_priv->display.load_csc_matrix)
		dev_priv->display.load_csc_matrix(crtc);
}

void intel_color_init(struct drm_crtc *crtc)
{
	struct drm_device *dev = crtc->dev;
	struct drm_i915_private *dev_priv = dev->dev_private;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int i;

	drm_mode_crtc_set_gamma_size(crtc, 256);
	for (i = 0; i < 256; i++) {
		intel_crtc->lut_r[i] = i;
		intel_crtc->lut_g[i] = i;
		intel_crtc->lut_b[i] = i;
	}

	if (IS_CHERRYVIEW(dev)) {
		dev_priv->display.load_degamma_lut =
			cherryview_load_degamma_lut;
		dev_priv->display.load_gamma_lut = cherryview_load_gamma_lut;
		dev_priv->display.load_csc_matrix = cherryview_load_csc_matrix;
	} else if (IS_BROADWELL(dev) || IS_SKYLAKE(dev) ||
		   IS_BROXTON(dev) || IS_KABYLAKE(dev)) {
		dev_priv->display.load_degamma_lut = broadwell_load_degamma_lut;
		dev_priv->display.load_gamma_lut = broadwell_load_gamma_lut;
		dev_priv->display.load_csc_matrix = i9xx_load_csc_matrix;
	} else {
		dev_priv->display.load_csc_matrix = i9xx_load_csc_matrix;
	}

	if (INTEL_INFO(dev)->color.degamma_lut_size != 0 &&
	    INTEL_INFO(dev)->color.gamma_lut_size != 0) {
		WARN_ON(!dev_priv->display.load_degamma_lut ||
			!dev_priv->display.load_gamma_lut ||
			!dev_priv->display.load_csc_matrix);
		drm_helper_crtc_enable_color_mgmt(crtc,
					INTEL_INFO(dev)->color.degamma_lut_size,
					INTEL_INFO(dev)->color.gamma_lut_size);
	}
}
