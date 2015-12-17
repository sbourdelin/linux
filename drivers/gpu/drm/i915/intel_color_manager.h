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
#include <drm/drmP.h>
#include <drm/drm_crtc_helper.h>
#include "i915_drv.h"

/* Color management bit utilities */
#define GET_BIT_MASK(n) ((1 << n) - 1)

/* Read bits of a word from bit no. 'start'(lsb) till 'n' bits */
#define GET_BITS(x, start, nbits) ((x >> start) & GET_BIT_MASK(nbits))

/* Round off by adding 1 to the immediate lower bit */
#define GET_BITS_ROUNDOFF(x, start, nbits) \
	((GET_BITS(x, start, (nbits + 1)) + 1) >> 1)

/* Clear bits of a word from bit no. 'start' till nbits */
#define CLEAR_BITS(x, start, nbits) ( \
		x &= ~((GET_BIT_MASK(nbits) << start)))

/* Write bit_pattern of no_bits bits in a target word */
#define SET_BITS(target, bit_pattern, start_bit, no_bits) \
		do { \
			CLEAR_BITS(target, start_bit, no_bits); \
			target |= (bit_pattern << start_bit);  \
		} while (0)

/* CHV */
#define CHV_10BIT_GAMMA_MAX_VALS		257
#define CHV_DEGAMMA_MAX_VALS                   65

/* No of coeff for disabling gamma is 0 */
#define GAMMA_DISABLE_VALS			0

/* Gamma on CHV */
#define CHV_10BIT_GAMMA_MAX_VALS               257
#define CHV_8BIT_GAMMA_MAX_VALS                256
#define CHV_10BIT_GAMMA_MSB_SHIFT              6
#define CHV_GAMMA_SHIFT_GREEN                  16
#define CHV_MAX_GAMMA                          ((1 << 24) - 1)

/*
 * CSC on CHV
 * Fractional part is 32 bit, and we need only 12 MSBs for programming
 * into registers. ROUNDOFF is required to minimize loss of precision.
 */
#define CHV_CSC_FRACT_ROUNDOFF                 (1 << 19)
/*
 * CSC values are 64-bit values. For CHV, the maximum CSC value that
 * user can program is 7.99999..., which can be represented in fixed point
 * S31.32 format like this, with all fractional bits as 1
 */
#define CHV_CSC_COEFF_MAX                      0x00000007FFFFFFFF
#define CHV_CSC_COEFF_SHIFT                    32
#define CHV_CSC_COEFF_INT_SHIFT                12
#define CSC_COEFF_SIGN                         (1 << 15)
#define CHV_CSC_COEFF_FRACT_SHIFT              20
#define CSC_MAX_VALS                           9

/* Degamma on CHV */
#define CHV_DEGAMMA_MSB_SHIFT                  2
#define CHV_DEGAMMA_GREEN_SHIFT                16

/* CHV CGM Block */
#define CGM_GAMMA_EN                           (1 << 2)
#define CGM_CSC_EN                             (1 << 1)
#define CGM_DEGAMMA_EN                         (1 << 0)

/* BDW CSC */
/* 1.0000000 in S31.32 format */
#define BDW_CSC_COEFF_UNITY_VAL	0x100000000
/* 3.9921875 in S31.32 format */
#define BDW_CSC_COEFF_MAX_VAL	0x3FE000000
/*-4.000000 in S31.32 format */
#define BDW_CSC_COEFF_MIN_VAL	0xFFFFFFFC00000000

/* Gamma on BDW */
#define BDW_SPLITGAMMA_MAX_VALS                512
#define BDW_8BIT_GAMMA_MAX_VALS		256
#define BDW_10BIT_GAMMA_MAX_VALS		1024
#define BDW_12BIT_GAMMA_MAX_VALS		513
#define BDW_MAX_GAMMA                         ((1 << 24) - 1)
#define BDW_INDEX_AUTO_INCREMENT               (1 << 15)
#define BDW_INDEX_SPLIT_MODE                   (1 << 31)

/* Degamma on BDW */
#define BDW_DEGAMMA_MAX_VALS                   512
