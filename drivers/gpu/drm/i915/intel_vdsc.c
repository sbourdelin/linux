/*
 * Copyright © 2018 Intel Corporation
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
 * Author: Gaurav K Singh <gaurav.k.singh@intel.com>
 */

#include <drm/drmP.h>
#include <drm/i915_drm.h>
#include "i915_drv.h"
#include "intel_drv.h"

enum ROW_INDEX_BPP {
	ROW_INDEX_INVALID = 127,
	ROW_INDEX_6BPP = 0,
	ROW_INDEX_8BPP,
	ROW_INDEX_10BPP,
	ROW_INDEX_12BPP,
	ROW_INDEX_15BPP,
	MAX_ROW_INDEX
};

enum COLUMN_INDEX_BPC {
	COLUMN_INDEX_INVALID = 127,
	COLUMN_INDEX_8BPC = 0,
	COLUMN_INDEX_10BPC,
	COLUMN_INDEX_12BPC,
	COLUMN_INDEX_14BPC,
	COLUMN_INDEX_16BPC,
	MAX_COLUMN_INDEX
};

#define SWAP_TWO_BYTES(x) (unsigned short)(((x >> 8) & 0xFF) | \
						((x << 8) & 0xFF00))

#define TWOS_COMPLEMENT(x) (unsigned char)((~(x) + 1) & 0x3F)

/* From DSC_v1.11 spec, rc_parameter_Set syntax element typically constant */
static unsigned long rc_buf_thresh[] = {
	896, 1792, 2688, 3584, 4480, 5376, 6272, 6720, 7168, 7616,
	7744, 7872, 8000, 8064
};

/*
 * From DSC_v1.11 spec
 * Selected Rate Control Related Parameter Recommended Values
 */
static struct rc_parameters rc_params[][MAX_COLUMN_INDEX] = {
{
	/* 6BPP/8BPC */
	{ 768, 15, 6144, 3, 13, 11, 11,
	{ { 0, 4, 0 }, { 1, 6, TWOS_COMPLEMENT(2) },
	{ 3, 8, TWOS_COMPLEMENT(2) }, { 4, 8, TWOS_COMPLEMENT(4) },
	{ 5, 9, TWOS_COMPLEMENT(6) }, { 5, 9, TWOS_COMPLEMENT(6) },
	{ 6, 9, TWOS_COMPLEMENT(6) }, { 6, 10, TWOS_COMPLEMENT(8) },
	{ 7, 11, TWOS_COMPLEMENT(8) }, { 8, 12, TWOS_COMPLEMENT(10) },
	{ 9, 12, TWOS_COMPLEMENT(10) }, { 10, 12, TWOS_COMPLEMENT(12) },
	{ 10, 12, TWOS_COMPLEMENT(12) }, { 11, 12, TWOS_COMPLEMENT(12) },
	{ 13, 14, TWOS_COMPLEMENT(12) } } },
	/* 6BPP/10BPC */
	{ 768, 15, 6144, 7, 17, 15, 15,
	{ { 0, 8, 0 }, { 3, 10, TWOS_COMPLEMENT(2) },
	{ 7, 12, TWOS_COMPLEMENT(2) }, { 8, 12, TWOS_COMPLEMENT(4) },
	{ 9, 13, TWOS_COMPLEMENT(6) }, { 9, 13, TWOS_COMPLEMENT(6) },
	{ 10, 13, TWOS_COMPLEMENT(6) }, { 10, 14, TWOS_COMPLEMENT(8) },
	{ 11, 15, TWOS_COMPLEMENT(8) }, { 12, 16, TWOS_COMPLEMENT(10) },
	{ 13, 16, TWOS_COMPLEMENT(10) }, { 14, 16, TWOS_COMPLEMENT(12) },
	{ 14, 16, TWOS_COMPLEMENT(12) }, { 15, 16, TWOS_COMPLEMENT(12) },
	{ 17, 18, TWOS_COMPLEMENT(12) } } },
	/* 6BPP/12BPC */
	{ 768, 15, 6144, 11, 21, 19, 19,
	{ { 0, 12, 0 }, { 5, 14, TWOS_COMPLEMENT(2) },
	{ 11, 16, TWOS_COMPLEMENT(2) }, { 12, 16, TWOS_COMPLEMENT(4) },
	{ 13, 17, TWOS_COMPLEMENT(6) }, { 13, 17, TWOS_COMPLEMENT(6) },
	{ 14, 17, TWOS_COMPLEMENT(6) }, { 14, 18, TWOS_COMPLEMENT(8) },
	{ 15, 19, TWOS_COMPLEMENT(8) }, { 16, 20, TWOS_COMPLEMENT(10) },
	{ 17, 20, TWOS_COMPLEMENT(10) }, { 18, 20, TWOS_COMPLEMENT(12) },
	{ 18, 20, TWOS_COMPLEMENT(12) }, { 19, 20, TWOS_COMPLEMENT(12) },
	{ 21, 22, TWOS_COMPLEMENT(12) } } },
	/* 6BPP/14BPC */
	{ 768, 15, 6144, 15, 25, 23, 27,
	{ { 0, 16, 0 }, { 7, 18, TWOS_COMPLEMENT(2) },
	{ 15, 20, TWOS_COMPLEMENT(2) }, { 16, 20, TWOS_COMPLEMENT(4) },
	{ 17, 21, TWOS_COMPLEMENT(6) }, { 17, 21, TWOS_COMPLEMENT(6) },
	{ 18, 21, TWOS_COMPLEMENT(6) }, { 18, 22, TWOS_COMPLEMENT(8) },
	{ 19, 23, TWOS_COMPLEMENT(8) }, { 20, 24, TWOS_COMPLEMENT(10) },
	{ 21, 24, TWOS_COMPLEMENT(10) }, { 22, 24, TWOS_COMPLEMENT(12) },
	{ 22, 24, TWOS_COMPLEMENT(12) }, { 23, 24, TWOS_COMPLEMENT(12) },
	{ 25, 26, TWOS_COMPLEMENT(12) } } },
	/* 6BPP/16BPC */
	{ 768, 15, 6144, 19, 29, 27, 27,
	{ { 0, 20, 0 }, { 9, 22, TWOS_COMPLEMENT(2) },
	{ 19, 24, TWOS_COMPLEMENT(2) }, { 20, 24, TWOS_COMPLEMENT(4) },
	{ 21, 25, TWOS_COMPLEMENT(6) }, { 21, 25, TWOS_COMPLEMENT(6) },
	{ 22, 25, TWOS_COMPLEMENT(6) }, { 22, 26, TWOS_COMPLEMENT(8) },
	{ 23, 27, TWOS_COMPLEMENT(8) }, { 24, 28, TWOS_COMPLEMENT(10) },
	{ 25, 28, TWOS_COMPLEMENT(10) }, { 26, 28, TWOS_COMPLEMENT(12) },
	{ 26, 28, TWOS_COMPLEMENT(12) }, { 27, 28, TWOS_COMPLEMENT(12) },
	{ 29, 30, TWOS_COMPLEMENT(12) } } },
},
{
	/* 8BPP/8BPC */
	{ 512, 12, 6144, 3, 12, 11, 11,
	{ { 0, 4, 2 }, { 0, 4, 0 }, { 1, 5, 0 }, { 1, 6, TWOS_COMPLEMENT(2) },
	{ 3, 7, TWOS_COMPLEMENT(4) }, { 3, 7, TWOS_COMPLEMENT(6) },
	{ 3, 7, TWOS_COMPLEMENT(8) }, { 3, 8, TWOS_COMPLEMENT(8) },
	{ 3, 9, TWOS_COMPLEMENT(8) }, { 3, 10, TWOS_COMPLEMENT(10) },
	{ 5, 11, TWOS_COMPLEMENT(10) }, { 5, 12, TWOS_COMPLEMENT(12) },
	{ 5, 13, TWOS_COMPLEMENT(12) }, { 7, 13, TWOS_COMPLEMENT(12) },
	{ 13, 15, TWOS_COMPLEMENT(12) } } },
	/* 8BPP/10BPC */
	{ 512, 12, 6144, 7, 16, 15, 15,
	{ { 0, 4, 2 }, { 4, 8, 0 }, { 5, 9, 0 }, { 5, 10, TWOS_COMPLEMENT(2) },
	{ 7, 11, TWOS_COMPLEMENT(4) }, { 7, 11, TWOS_COMPLEMENT(6) },
	{ 7, 11, TWOS_COMPLEMENT(8) }, { 7, 12, TWOS_COMPLEMENT(8) },
	{ 7, 13, TWOS_COMPLEMENT(8) }, { 7, 14, TWOS_COMPLEMENT(10) },
	{ 9, 15, TWOS_COMPLEMENT(10) }, { 9, 16, TWOS_COMPLEMENT(12) },
	{ 9, 17, TWOS_COMPLEMENT(12) }, { 11, 17, TWOS_COMPLEMENT(12) },
	{ 17, 19, TWOS_COMPLEMENT(12) } } },
	/* 8BPP/12BPC */
	{ 512, 12, 6144, 11, 20, 19, 19,
	{ { 0, 12, 2 }, { 4, 12, 0 }, { 9, 13, 0 },
	{ 9, 14, TWOS_COMPLEMENT(2) }, { 11, 15, TWOS_COMPLEMENT(4) },
	{ 11, 15, TWOS_COMPLEMENT(6) }, { 11, 15, TWOS_COMPLEMENT(8) },
	{ 11, 16, TWOS_COMPLEMENT(8) }, { 11, 17, TWOS_COMPLEMENT(8) },
	{ 11, 18, TWOS_COMPLEMENT(10) }, { 13, 19, TWOS_COMPLEMENT(10) },
	{ 13, 20, TWOS_COMPLEMENT(12) }, { 13, 21, TWOS_COMPLEMENT(12) },
	{ 15, 21, TWOS_COMPLEMENT(12) }, { 21, 23, TWOS_COMPLEMENT(12) } } },
	/* 8BPP/14BPC */
	{ 512, 12, 6144, 15, 24, 23, 23, { { 0, 12, 0 }, { 5, 13, 0 },
	{ 11, 15, 0 }, { 12, 17, TWOS_COMPLEMENT(2) },
	{ 15, 19, TWOS_COMPLEMENT(4) }, { 15, 19, TWOS_COMPLEMENT(6) },
	{ 15, 19, TWOS_COMPLEMENT(8) }, { 15, 20, TWOS_COMPLEMENT(8) },
	{ 15, 21, TWOS_COMPLEMENT(8) }, { 15, 22, TWOS_COMPLEMENT(10) },
	{ 17, 22, TWOS_COMPLEMENT(10) }, { 17, 23, TWOS_COMPLEMENT(12) },
	{ 17, 23, TWOS_COMPLEMENT(12) }, { 21, 24, TWOS_COMPLEMENT(12) },
	{ 24, 25, TWOS_COMPLEMENT(12) } } },
	/* 8BPP/16BPC */
	{ 512, 12, 6144, 19, 28, 27, 27, { { 0, 12, 2 }, { 6, 14, 0 },
	{ 13, 17, 0 }, { 15, 20, TWOS_COMPLEMENT(2) },
	{ 19, 23, TWOS_COMPLEMENT(4) }, { 19, 23, TWOS_COMPLEMENT(6) },
	{ 19, 23, TWOS_COMPLEMENT(8) }, { 19, 24, TWOS_COMPLEMENT(8) },
	{ 19, 25, TWOS_COMPLEMENT(8) }, { 19, 26, TWOS_COMPLEMENT(10) },
	{ 21, 26, TWOS_COMPLEMENT(10) }, { 21, 27, TWOS_COMPLEMENT(12) },
	{ 21, 27, TWOS_COMPLEMENT(12) }, { 25, 28, TWOS_COMPLEMENT(12) },
	{ 28, 29, TWOS_COMPLEMENT(12) } } },
},
{
	/* 10BPP/8BPC */
	{ 410, 15, 5632, 3, 12, 11, 11, { { 0, 3, 2 }, { 0, 4, 0 },
	{ 1, 5, 0 }, { 2, 6, TWOS_COMPLEMENT(2) }, { 3, 7, TWOS_COMPLEMENT(4) },
	{ 3, 7, TWOS_COMPLEMENT(6) }, { 3, 7, TWOS_COMPLEMENT(8) },
	{ 3, 8, TWOS_COMPLEMENT(8) }, { 3, 9, TWOS_COMPLEMENT(8) },
	{ 3, 9, TWOS_COMPLEMENT(10) }, { 5, 10, TWOS_COMPLEMENT(10) },
	{ 5, 10, TWOS_COMPLEMENT(10) }, { 5, 11, TWOS_COMPLEMENT(12) },
	{ 7, 11, TWOS_COMPLEMENT(12) }, { 11, 12, TWOS_COMPLEMENT(12) } } },
	/* 10BPP/10BPC */
	{ 410, 15, 5632, 7, 16, 15, 15, { { 0, 7, 2 }, { 4, 8, 0 },
	{ 5, 9, 0 }, { 6, 10, TWOS_COMPLEMENT(2) },
	{ 7, 11, TWOS_COMPLEMENT(4) }, { 7, 11, TWOS_COMPLEMENT(6) },
	{ 7, 11, TWOS_COMPLEMENT(8) }, { 7, 12, TWOS_COMPLEMENT(8) },
	{ 7, 13, TWOS_COMPLEMENT(8) }, { 7, 13, TWOS_COMPLEMENT(10) },
	{ 9, 14, TWOS_COMPLEMENT(10) }, { 9, 14, TWOS_COMPLEMENT(10) },
	{ 9, 15, TWOS_COMPLEMENT(12) }, { 11, 15, TWOS_COMPLEMENT(12) },
	{ 15, 16, TWOS_COMPLEMENT(12) } } },
	/* 10BPP/12BPC */
	{ 410, 15, 5632, 11, 20, 19, 19, { { 0, 11, 2 }, { 4, 12, 0 },
	{ 9, 13, 0 }, { 10, 14, TWOS_COMPLEMENT(2) },
	{ 11, 15, TWOS_COMPLEMENT(4) }, { 11, 15, TWOS_COMPLEMENT(6) },
	{ 11, 15, TWOS_COMPLEMENT(8) }, { 11, 16, TWOS_COMPLEMENT(8) },
	{ 11, 17, TWOS_COMPLEMENT(8) }, { 11, 17, TWOS_COMPLEMENT(10) },
	{ 13, 18, TWOS_COMPLEMENT(10) }, { 13, 18, TWOS_COMPLEMENT(10) },
	{ 13, 19, TWOS_COMPLEMENT(12) }, { 15, 19, TWOS_COMPLEMENT(12) },
	{ 19, 20, TWOS_COMPLEMENT(12) } } },
	/* 10BPP/14BPC */
	{ 410, 15, 5632, 15, 24, 23, 23, { { 0, 11, 2 }, { 5, 13, 0 },
	{ 11, 15, 0 }, { 13, 18, TWOS_COMPLEMENT(2) },
	{ 15, 19, TWOS_COMPLEMENT(4) }, { 15, 19, TWOS_COMPLEMENT(6) },
	{ 15, 19, TWOS_COMPLEMENT(8) }, { 15, 20, TWOS_COMPLEMENT(8) },
	{ 15, 21, TWOS_COMPLEMENT(8) }, { 15, 21, TWOS_COMPLEMENT(10) },
	{ 17, 22, TWOS_COMPLEMENT(10) }, { 17, 22, TWOS_COMPLEMENT(10) },
	{ 17, 23, TWOS_COMPLEMENT(12) }, { 19, 23, TWOS_COMPLEMENT(12) },
	{ 23, 24, TWOS_COMPLEMENT(12) } } },
	/* 10BPP/16BPC */
	{ 410, 15, 5632, 19, 28, 27, 27, { { 0, 11, 2 }, { 6, 14, 0 },
	{ 13, 17, 0 }, { 16, 20, TWOS_COMPLEMENT(2) },
	{ 19, 23, TWOS_COMPLEMENT(4) }, { 19, 23, TWOS_COMPLEMENT(6) },
	{ 19, 23, TWOS_COMPLEMENT(8) }, { 19, 24, TWOS_COMPLEMENT(8) },
	{ 19, 25, TWOS_COMPLEMENT(8) }, { 19, 25, TWOS_COMPLEMENT(10) },
	{ 21, 26, TWOS_COMPLEMENT(10) }, { 21, 26, TWOS_COMPLEMENT(10) },
	{ 21, 27, TWOS_COMPLEMENT(12) }, { 23, 27, TWOS_COMPLEMENT(12) },
	{ 27, 28, TWOS_COMPLEMENT(12) } } },
},
{
	/* 12BPP/8BPC */
	{ 341, 15, 2048, 3, 12, 11, 11, { { 0, 2, 2 }, { 0, 4, 0 },
	{ 1, 5, 0 }, { 1, 6, TWOS_COMPLEMENT(2) }, { 3, 7, TWOS_COMPLEMENT(4) },
	{ 3, 7, TWOS_COMPLEMENT(6) }, { 3, 7, TWOS_COMPLEMENT(8) },
	{ 3, 8, TWOS_COMPLEMENT(8) }, { 3, 9, TWOS_COMPLEMENT(8) },
	{ 3, 10, TWOS_COMPLEMENT(10) }, { 5, 11, TWOS_COMPLEMENT(10) },
	{ 5, 12, TWOS_COMPLEMENT(12) }, { 5, 13, TWOS_COMPLEMENT(12) },
	{ 7, 13, TWOS_COMPLEMENT(12) }, { 13, 15, TWOS_COMPLEMENT(12) } } },
	/* 12BPP/10BPC */
	{ 341, 15, 2048, 7, 16, 15, 15, { { 0, 2, 2 }, { 2, 5, 0 },
	{ 3, 7, 0 }, { 4, 8, TWOS_COMPLEMENT(2) }, { 6, 9, TWOS_COMPLEMENT(4) },
	{ 7, 10, TWOS_COMPLEMENT(6) }, { 7, 11, TWOS_COMPLEMENT(8) },
	{ 7, 12, TWOS_COMPLEMENT(8) }, { 7, 13, TWOS_COMPLEMENT(8) },
	{ 7, 14, TWOS_COMPLEMENT(10) }, { 9, 15, TWOS_COMPLEMENT(10) },
	{ 9, 16, TWOS_COMPLEMENT(12) }, { 9, 17, TWOS_COMPLEMENT(12) },
	{ 11, 17, TWOS_COMPLEMENT(12) }, { 17, 19, TWOS_COMPLEMENT(12) } } },
	/* 12BPP/12BPC */
	{ 341, 15, 2048, 11, 20, 19, 19, { { 0, 6, 2 }, { 4, 9, 0 },
	{ 7, 11, 0 }, { 8, 12, TWOS_COMPLEMENT(2) },
	{ 10, 13, TWOS_COMPLEMENT(4) }, { 11, 14, TWOS_COMPLEMENT(6) },
	{ 11, 15, TWOS_COMPLEMENT(8) }, { 11, 16, TWOS_COMPLEMENT(8) },
	{ 11, 17, TWOS_COMPLEMENT(8) }, { 11, 18, TWOS_COMPLEMENT(10) },
	{ 13, 19, TWOS_COMPLEMENT(10) }, { 13, 20, TWOS_COMPLEMENT(12) },
	{ 13, 21, TWOS_COMPLEMENT(12) }, { 15, 21, TWOS_COMPLEMENT(12) },
	{ 21, 23, TWOS_COMPLEMENT(12) } } },
	/* 12BPP/14BPC */
	{ 341, 15, 2048, 15, 24, 23, 23, { { 0, 6, 2 }, { 7, 10, 0 },
	{ 9, 13, 0 }, { 11, 16, TWOS_COMPLEMENT(2) },
	{ 14, 17, TWOS_COMPLEMENT(4) }, { 15, 18, TWOS_COMPLEMENT(6) },
	{ 15, 19, TWOS_COMPLEMENT(8) }, { 15, 20, TWOS_COMPLEMENT(8) },
	{ 15, 20, TWOS_COMPLEMENT(8) }, { 15, 21, TWOS_COMPLEMENT(10) },
	{ 17, 21, TWOS_COMPLEMENT(10) }, { 17, 21, TWOS_COMPLEMENT(12) },
	{ 17, 21, TWOS_COMPLEMENT(12) }, { 19, 22, TWOS_COMPLEMENT(12) },
	{ 22, 23, TWOS_COMPLEMENT(12) } } },
	/* 12BPP/16BPC */
	{ 341, 15, 2048, 19, 28, 27, 27, { { 0, 6, 2 }, { 6, 11, 0 },
	{ 11, 15, 0 }, { 14, 18, TWOS_COMPLEMENT(2) },
	{ 18, 21, TWOS_COMPLEMENT(4) }, { 19, 22, TWOS_COMPLEMENT(6) },
	{ 19, 23, TWOS_COMPLEMENT(8) }, { 19, 24, TWOS_COMPLEMENT(8) },
	{ 19, 24, TWOS_COMPLEMENT(8) }, { 19, 25, TWOS_COMPLEMENT(10) },
	{ 21, 25, TWOS_COMPLEMENT(10) }, { 21, 25, TWOS_COMPLEMENT(12) },
	{ 21, 25, TWOS_COMPLEMENT(12) }, { 23, 26, TWOS_COMPLEMENT(12) },
	{ 26, 27, TWOS_COMPLEMENT(12) } } },
},
{
	/* 15BPP/8BPC */
	{ 273, 15, 2048, 3, 12, 11, 11, { { 0, 0, 10 }, { 0, 1, 8 },
	{ 0, 1, 6 }, { 0, 2, 4 }, { 1, 2, 2 }, { 1, 3, 0 },
	{ 1, 3, TWOS_COMPLEMENT(2) }, { 2, 4, TWOS_COMPLEMENT(4) },
	{ 2, 5, TWOS_COMPLEMENT(6) }, { 3, 5, TWOS_COMPLEMENT(8) },
	{ 4, 6, TWOS_COMPLEMENT(10) }, { 4, 7, TWOS_COMPLEMENT(10) },
	{ 5, 7, TWOS_COMPLEMENT(12) }, { 7, 8, TWOS_COMPLEMENT(12) },
	{ 8, 9, TWOS_COMPLEMENT(12) } } },
	/* 15BPP/10BPC */
	{ 273, 15, 2048, 7, 16, 15, 15, { { 0, 2, 10 }, { 2, 5, 8 },
	{ 3, 5, 6 }, { 4, 6, 4 }, { 5, 6, 2 }, { 5, 7, 0 },
	{ 5, 7, TWOS_COMPLEMENT(2) }, { 6, 8, TWOS_COMPLEMENT(4) },
	{ 6, 9, TWOS_COMPLEMENT(6) }, { 7, 9, TWOS_COMPLEMENT(8) },
	{ 8, 10, TWOS_COMPLEMENT(10) }, { 8, 11, TWOS_COMPLEMENT(10) },
	{ 9, 11, TWOS_COMPLEMENT(12) }, { 11, 12, TWOS_COMPLEMENT(12) },
	{ 12, 13, TWOS_COMPLEMENT(12) } } },
	/* 15BPP/12BPC */
	{ 273, 15, 2048, 11, 20, 19, 19, { { 0, 4, 10 }, { 2, 7, 8 },
	{ 4, 9, 6 }, { 6, 11, 4 }, { 9, 11, 2 }, { 9, 11, 0 },
	{ 9, 12, TWOS_COMPLEMENT(2) }, { 10, 12, TWOS_COMPLEMENT(4) },
	{ 11, 13, TWOS_COMPLEMENT(6) }, { 11, 13, TWOS_COMPLEMENT(8) },
	{ 12, 14, TWOS_COMPLEMENT(10) }, { 13, 15, TWOS_COMPLEMENT(10) },
	{ 13, 15, TWOS_COMPLEMENT(12) }, { 15, 16, TWOS_COMPLEMENT(12) },
	{ 16, 17, TWOS_COMPLEMENT(12) } } },
	/* 15BPP/14BPC */
	{ 273, 15, 2048, 15, 24, 23, 23, { { 0, 4, 10 }, { 3, 8, 8 },
	{ 6, 11, 6 }, { 9, 14, 4 }, { 13, 15, 2 }, { 13, 15, 0 },
	{ 13, 16, TWOS_COMPLEMENT(2) }, { 14, 16, TWOS_COMPLEMENT(4) },
	{ 15, 17, TWOS_COMPLEMENT(6) }, { 15, 17, TWOS_COMPLEMENT(8) },
	{ 16, 18, TWOS_COMPLEMENT(10) }, { 17, 19, TWOS_COMPLEMENT(10) },
	{ 17, 19, TWOS_COMPLEMENT(12) }, { 19, 20, TWOS_COMPLEMENT(12) },
	{ 20, 21, TWOS_COMPLEMENT(12) } } },
	/* 15BPP/16BPC */
	{ 273, 15, 2048, 19, 28, 27, 27, { { 0, 4, 10 }, { 4, 9, 8 },
	{ 8, 13, 6 }, { 12, 17, 4 }, { 17, 19, 2 }, { 17, 20, 0 },
	{ 17, 20, TWOS_COMPLEMENT(2) }, { 18, 20, TWOS_COMPLEMENT(4) },
	{ 19, 21, TWOS_COMPLEMENT(6) }, { 19, 21, TWOS_COMPLEMENT(8) },
	{ 20, 22, TWOS_COMPLEMENT(10) }, { 21, 23, TWOS_COMPLEMENT(10) },
	{ 21, 23, TWOS_COMPLEMENT(12) }, { 23, 24, TWOS_COMPLEMENT(12) },
	{ 24, 25, TWOS_COMPLEMENT(12) } } }
}
};

static void intel_compute_rc_parameters(struct intel_dp *intel_dp)
{
	unsigned long groups_per_line = 0;
	unsigned long groups_total = 0;
	unsigned long num_extra_mux_bits = 0;
	unsigned long slice_bits = 0;
	unsigned long hrd_delay = 0;
	unsigned long final_scale = 0;
	unsigned long rbs_min = 0;
	struct vdsc_config *vdsc_cfg  =  &(intel_dp->compr_params.dsc_cfg);

	/* Number of groups used to code each line of a slice */
	groups_per_line = DIV_ROUND_UP(vdsc_cfg->slice_width, 3);

	/* chunksize = DIV_ROUND_UP(slicewidth*bitsperpixel/8) in Bytes */
	vdsc_cfg->chunk_size = DIV_ROUND_UP(vdsc_cfg->slice_width *
						vdsc_cfg->bits_per_pixel,
								(8 * 16));

	if (vdsc_cfg->convert_rgb)
		num_extra_mux_bits = 3 *
				(vdsc_cfg->mux_word_size +
				(4 * vdsc_cfg->bits_per_component + 4) - 2);
	else
// YCbCr
		num_extra_mux_bits = 3 * vdsc_cfg->mux_word_size +
				(4 * vdsc_cfg->bits_per_component + 4) +
				2 * (4 * vdsc_cfg->bits_per_component) - 2;
	/* Number of bits in one Slice */
	slice_bits = 8 * vdsc_cfg->chunk_size * vdsc_cfg->slice_height;

	while ((num_extra_mux_bits > 0) &&
		((slice_bits - num_extra_mux_bits) % vdsc_cfg->mux_word_size))
		num_extra_mux_bits--;

	if (groups_per_line < vdsc_cfg->initial_scale_value - 8)
		vdsc_cfg->initial_scale_value = groups_per_line + 8;

	if (vdsc_cfg->initial_scale_value > 8)
		vdsc_cfg->scale_decrement_interval = groups_per_line /
					(vdsc_cfg->initial_scale_value - 8);
	else
		vdsc_cfg->scale_decrement_interval = 4095;

	vdsc_cfg->final_offset = vdsc_cfg->rc_model_size -
				(vdsc_cfg->initial_xmit_delay *
				vdsc_cfg->bits_per_pixel + 8) / 16 +
				num_extra_mux_bits;

	if (vdsc_cfg->final_offset >= vdsc_cfg->rc_model_size) {
		DRM_ERROR("FinalOfs < RcModelSze.Increase InitialXmitDelay\n");
		return;
	}

	/* FinalScale, multiply by 8 to preserve 3 fractional bits */
	final_scale = (8 * vdsc_cfg->rc_model_size) /
			(vdsc_cfg->rc_model_size - vdsc_cfg->final_offset);
	if (vdsc_cfg->slice_height > 1)
		/*
		 * NflBpgOffset is 16 bit value with 11 fractional bits
		 * hence we multiply by 2^11 for preserving the
		 * fractional part
		 */
		vdsc_cfg->nfl_bpg_offset = DIV_ROUND_UP(
				(vdsc_cfg->first_line_bpg_Ofs << 11),
				(vdsc_cfg->slice_height - 1));
	else
		vdsc_cfg->nfl_bpg_offset = 0;

	/* 2^16 - 1 */
	if (vdsc_cfg->nfl_bpg_offset > 65535) {
		DRM_ERROR("NflBpgOffset is too large for this slice height\n");
		return;
	}

	/* Number of groups used to code the entire slice */
	groups_total = groups_per_line * vdsc_cfg->slice_height;

	/*
	 * slice_bpg_offset is 16 bit value with 11 fractional bits
	 * hence we multiply by 2^11 for preserving the fractional part
	 */
	vdsc_cfg->slice_bpg_offset = DIV_ROUND_UP(
			((vdsc_cfg->rc_model_size - vdsc_cfg->initial_offset +
			num_extra_mux_bits) << 11), groups_total);

	if (final_scale > 9) {
	/*
	 * ScaleIncrementInterval =
	 * finaloffset/((NflBpgOffset + SliceBpgOffset)*8(finalscale - 1.125))
	 * as (NflBpgOffset + SliceBpgOffset) has 11 bit fractional value,
	 * we need divide by 2^11 from pstDscCfg values
	 * ScaleIncrementInterval =
	 *finaloffset/((NflBpgOfset + SlicBpgOfset)/2^11*8(finalscale - 1.125))
	 * ScaleIncrementInterval =
	 *finaloffset*2^11/((NflBpgOfset + SlicBpgOfset)*8(finalscale - 1.125))
	 * ScaleIncrementInterval =
	 * finaloffset*2^11/((NflBpgOffset + SliceBpgOffset)*(8*finalscale - 9))
	 * as finalscale has 3 fractional bits stored we need to divide by 8.
	 * ScaleIncrementInterval =
	 *finaloffset*2^11/((NflBpgOffset + SliceBpgOffset)*(finalscale - 9))
	 */
		vdsc_cfg->scale_increment_interval =
				(vdsc_cfg->final_offset * (1 << 11)) /
						((vdsc_cfg->nfl_bpg_offset +
						vdsc_cfg->slice_bpg_offset)*
						(final_scale - 9));
	} else {
	/*
	 * If finalScaleValue is less than or equal to 9, a value of 0 should
	 * be used to disable the scale increment at the end of the slice
	 */
		vdsc_cfg->scale_increment_interval = 0;
	}

	if (vdsc_cfg->scale_increment_interval > 65535) {
		DRM_ERROR("ScaleIncrementInterval is large for slice height\n");
		return;
	}
	rbs_min = vdsc_cfg->rc_model_size - vdsc_cfg->initial_offset +
		(vdsc_cfg->initial_xmit_delay * vdsc_cfg->bits_per_pixel) /
		16 + groups_per_line * vdsc_cfg->first_line_bpg_Ofs;

	hrd_delay = DIV_ROUND_UP((rbs_min * 16), vdsc_cfg->bits_per_pixel);
	vdsc_cfg->rc_bits = (hrd_delay * vdsc_cfg->bits_per_pixel) / 16;
	vdsc_cfg->initial_dec_delay = hrd_delay - vdsc_cfg->initial_xmit_delay;
}

void intel_dp_compute_dsc_parameters(struct intel_dp *intel_dp)
{
	struct vdsc_config *vdsc_cfg  =  &(intel_dp->compr_params.dsc_cfg);
	unsigned long bits_per_pixel = 0;
	unsigned char i = 0;
	unsigned char row_index = 0;
	unsigned char column_index = 0;

	bits_per_pixel = vdsc_cfg->bits_per_pixel;

	/*
	 * rc_parameter_set  syntax elements typically
	 * constant across operating modes
	 */
	vdsc_cfg->rc_model_size = 8192;
	vdsc_cfg->rc_edge_factor = 6;
	vdsc_cfg->rc_tgt_offset_high = 3;
	vdsc_cfg->rc_tgt_offset_low = 3;

	for (i = 0; i < NUM_BUF_RANGES - 1; i++) {
		/*
		 * six 0s are appended to the lsb of each threshold value
		 * internally in h/w.
		 * Only 8 bits are allowed for programming RcBufThreshold,
		 * so we divide RcBufThreshold by 2^6
		 */
		vdsc_cfg->rc_buf_thresh[i] = rc_buf_thresh[i] / 64;
	}

	/* For 6bpp, RC Buffer threshold 12 and 13 need a different value. */
	if (bits_per_pixel == 6) {
		vdsc_cfg->rc_buf_thresh[12] = 0x7C;
		vdsc_cfg->rc_buf_thresh[13] = 0x7D;
	}

	switch (bits_per_pixel) {
	case 6:
		row_index = ROW_INDEX_6BPP;
		break;
	case 8:
		row_index = ROW_INDEX_8BPP;
		break;
	case 10:
		row_index = ROW_INDEX_10BPP;
		break;
	case 12:
		row_index = ROW_INDEX_12BPP;
		break;
	case 15:
		row_index = ROW_INDEX_15BPP;
		break;
	default:
		row_index = ROW_INDEX_INVALID;
		break;
	}

	if (row_index == ROW_INDEX_INVALID) {
		DRM_ERROR("Function:%s Unsupported BPP\n", __func__);
		return;
	}

	switch (vdsc_cfg->bits_per_component) {
	case 8:
		column_index = COLUMN_INDEX_8BPC;
		break;
	case 10:
		column_index = COLUMN_INDEX_10BPC;
		break;
	case 12:
		column_index = COLUMN_INDEX_12BPC;
		break;
	case 14:
		column_index = COLUMN_INDEX_14BPC;
		break;
	case 16:
		column_index = COLUMN_INDEX_16BPC;
		break;
	default:
		column_index = COLUMN_INDEX_INVALID;
		break;
	}

	if (column_index == COLUMN_INDEX_INVALID) {
		DRM_ERROR("Function:%s Unsupported BPC\n", __func__);
		return;
	}

	vdsc_cfg->first_line_bpg_Ofs =
			rc_params[row_index][column_index].first_line_bpg_Ofs;
	vdsc_cfg->initial_xmit_delay =
			rc_params[row_index][column_index].initial_xmit_delay;
	vdsc_cfg->initial_offset =
			rc_params[row_index][column_index].initial_offset;
	vdsc_cfg->flatness_minQp =
			rc_params[row_index][column_index].flatness_minQp;
	vdsc_cfg->flatness_maxQp =
			rc_params[row_index][column_index].flatness_maxQp;
	vdsc_cfg->rc_quant_incr_limit0 =
			rc_params[row_index][column_index].rc_quant_incr_limit0;
	vdsc_cfg->rc_quant_incr_limit1 =
			rc_params[row_index][column_index].rc_quant_incr_limit1;

	for (i = 0; i < NUM_BUF_RANGES; i++) {
		vdsc_cfg->rc_range_params[i].range_min_qp =
	rc_params[row_index][column_index].rc_range_params[i].range_min_qp;
		vdsc_cfg->rc_range_params[i].range_max_qp =
	rc_params[row_index][column_index].rc_range_params[i].range_max_qp;
		vdsc_cfg->rc_range_params[i].range_bpg_offset =
	rc_params[row_index][column_index].rc_range_params[i].range_bpg_offset;
	}

	if (vdsc_cfg->initial_offset >= vdsc_cfg->rc_model_size) {
		DRM_ERROR("Initial Offset is greater than RC Model Size\n");
		return;
	}

	/*
	 * BitsPerComponent value determines mux_word_size:
	 * When BitsPerComponent is 12bpc, muxWordSize will be equal to 64 bits
	 * When BitsPerComponent is 8 or 10bpc, muxWordSize will be equal to
	 * 48 bits
	 */
	if (vdsc_cfg->bits_per_component <= 10)
		vdsc_cfg->mux_word_size = 48;
	else
		vdsc_cfg->mux_word_size = 64;

	/*
	 * InitialScaleValue is a 6 bit value with 3 fractional bits (U3.3)
	 * In order to preserve the fractional part multiply numerator by 2^3
	 */
	vdsc_cfg->initial_scale_value = (8 * vdsc_cfg->rc_model_size) /
			(vdsc_cfg->rc_model_size - vdsc_cfg->initial_offset);

	intel_compute_rc_parameters(intel_dp);

}

void populate_pps_sdp_for_sink(struct intel_encoder *encoder,
				struct intel_crtc_state *crtc_state,
				struct picture_parameters_set *pps_params)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&encoder->base);
	struct vdsc_config *vdsc_cfg  =  &(intel_dp->compr_params.dsc_cfg);
	unsigned long rc_range_parameters[NUM_BUF_RANGES];
	unsigned char i = 0;

	/* PPS0 */
	pps_params->major = (unsigned char)vdsc_cfg->dsc_version_major;
	pps_params->minor = (unsigned char)vdsc_cfg->dsc_version_minor;

	/* PPS1, PPS2 */
	pps_params->picture_params_set_identifier = 0;

	/* PPS3 */
	pps_params->line_buffer_depth = (unsigned char)vdsc_cfg->line_buf_depth;
	pps_params->bits_per_component =
				(unsigned char)vdsc_cfg->bits_per_component;

	/* PPS4,5 */
	pps_params->block_prediction_enable =
				(unsigned short)vdsc_cfg->block_pred_enable;
	pps_params->convert_RGB = (unsigned short)vdsc_cfg->convert_rgb;
	pps_params->enable422 = (unsigned short)vdsc_cfg->enable422;
	pps_params->vbr_mode = (unsigned short)vdsc_cfg->vbr_enable;
	pps_params->bpp_low = (unsigned short)(
				(vdsc_cfg->bits_per_pixel >> 8) & 0x3);
	pps_params->bpp_high = (unsigned short)(vdsc_cfg->bits_per_pixel &
									0xFF);

	/*
	 * The PPS structure is stored as per our hardware registers which
	 * are in little endian. When a value is assigned to a variable,
	 * Intel systems stores data in little endian.
	 * For e.g UINT16 a = 0x1234;
	 * 0x34 is stored at lower address followed by 0x12.
	 * Though, PPS packet to the panel must have big endian format for
	 * data spanning 2 bytes. According to that logic, swap the
	 * fields of the PPS packets that span more than one byte.
	 */

	/* PPS6,7 */
	pps_params->picture_height = SWAP_TWO_BYTES(vdsc_cfg->pic_height);

	/* PPS8,9 */
	pps_params->picture_width = SWAP_TWO_BYTES(vdsc_cfg->pic_width);

	/* PPS10,11 */
	pps_params->slice_height = SWAP_TWO_BYTES(vdsc_cfg->slice_height);

	/* PPS12,13 */
	pps_params->slice_width = SWAP_TWO_BYTES(vdsc_cfg->slice_width);

	/* PPS14,15 */
	pps_params->chunk_size = SWAP_TWO_BYTES(vdsc_cfg->chunk_size);

	/* PPS15,16 */
	pps_params->transmission_delay_low = (unsigned short)
					((vdsc_cfg->initial_xmit_delay >> 8) &
					0x3); //[9:8]
	pps_params->transmission_delay_high = (unsigned short)
					(vdsc_cfg->initial_xmit_delay & 0xFF);

	/* PPS18,19 */
	pps_params->initial_decode_delay =
			SWAP_TWO_BYTES(vdsc_cfg->initial_dec_delay);

	/* PPS20,21 */
	pps_params->initial_scale =
				(unsigned short)vdsc_cfg->initial_scale_value;

	/* PPS22,23 */
	pps_params->scale_increment_interval =
			SWAP_TWO_BYTES(vdsc_cfg->scale_increment_interval);

	/* PPS24,25 */
	pps_params->scale_decrement_low = (unsigned short)(
			(vdsc_cfg->scale_decrement_interval >> 8) & 0xF);
	pps_params->scale_decrement_high = (unsigned short)(
				vdsc_cfg->scale_decrement_interval & 0xFF);

	/* PPS26,27 */
	pps_params->bpg_offset = (unsigned short)vdsc_cfg->first_line_bpg_Ofs;

	/* PPS28,29 */
	pps_params->nfl_bpg_offset = SWAP_TWO_BYTES(vdsc_cfg->nfl_bpg_offset);

	/* PPS30,31 */
	pps_params->slice_bpg_offset =
				SWAP_TWO_BYTES(vdsc_cfg->slice_bpg_offset);

	/* PPS32,33 */
	pps_params->initial_offset = SWAP_TWO_BYTES(vdsc_cfg->initial_offset);

	/* PPS34,35 */
	pps_params->final_offset = SWAP_TWO_BYTES(vdsc_cfg->final_offset);

	/* PPS36 */
	pps_params->flatness_min_qp = (unsigned char)vdsc_cfg->flatness_minQp;

	/* PPS37 */
	pps_params->flatness_max_qp = (unsigned char)vdsc_cfg->flatness_maxQp;

	/* PPS38,39 */
	pps_params->rc_model_size = SWAP_TWO_BYTES(vdsc_cfg->rc_model_size);

	/* PPS40 */
	pps_params->edge_factor = (unsigned char)vdsc_cfg->rc_edge_factor;

	/* PPS41 */
	pps_params->incr_limit0 = (unsigned char)vdsc_cfg->rc_quant_incr_limit0;

	/* PPS42 */
	pps_params->incr_limit1 = (unsigned char)vdsc_cfg->rc_quant_incr_limit1;

	/* PPS43 */
	pps_params->low = (unsigned char)vdsc_cfg->rc_tgt_offset_low;
	pps_params->high = (unsigned char)vdsc_cfg->rc_tgt_offset_high;

	/* PPS44 to PPS57 */
	pps_params->rc_buffer_threshold0 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[0];
	pps_params->rc_buffer_threshold1 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[1];
	pps_params->rc_buffer_threshold2 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[2];
	pps_params->rc_buffer_threshold3 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[3];
	pps_params->rc_buffer_threshold4 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[4];
	pps_params->rc_buffer_threshold5 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[5];
	pps_params->rc_buffer_threshold6 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[6];
	pps_params->rc_buffer_threshold7 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[7];
	pps_params->rc_buffer_threshold8 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[8];
	pps_params->rc_buffer_threshold9 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[9];
	pps_params->rc_buffer_threshold10 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[10];
	pps_params->rc_buffer_threshold11 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[11];
	pps_params->rc_buffer_threshold12 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[12];
	pps_params->rc_buffer_threshold13 =
				(unsigned char)vdsc_cfg->rc_buf_thresh[13];

	/*
	 * For each RC range parameter, we need to do below steps:
	 * For source programming the order is
	 * ((offset << 10) | (max << 5) | min))
	 * For sink programming the order is
	 * ((min << 11) | (max << 6) | offset))
	 */
	for (i = 0; i < NUM_BUF_RANGES; i++) {
		rc_range_parameters[i] = (unsigned short)(
		(vdsc_cfg->rc_range_params[i].range_min_qp << 11) |
		(vdsc_cfg->rc_range_params[i].range_max_qp << 6) |
		(vdsc_cfg->rc_range_params[i].range_bpg_offset));
	}

	/*
	 * Also while sending to the sink, we need to send in big endian order
	 * So, swap the two bytes of data after above operations.
	 * NOTE: The order of the min,max and offset field is not explicitly
	 * called out in DSC spec yet. We are following this order based on
	 * the VESA C Model implementation and the expectations from panel
	 * and Pipe 2D model.
	 */
	pps_params->rc_range_parameter0 =
					SWAP_TWO_BYTES(rc_range_parameters[0]);
	pps_params->rc_range_parameter1 =
					SWAP_TWO_BYTES(rc_range_parameters[1]);
	pps_params->rc_range_parameter2 =
					SWAP_TWO_BYTES(rc_range_parameters[2]);
	pps_params->rc_range_parameter3 =
					SWAP_TWO_BYTES(rc_range_parameters[3]);
	pps_params->rc_range_parameter4 =
					SWAP_TWO_BYTES(rc_range_parameters[4]);
	pps_params->rc_range_parameter5 =
					SWAP_TWO_BYTES(rc_range_parameters[5]);
	pps_params->rc_range_parameter6 =
					SWAP_TWO_BYTES(rc_range_parameters[6]);
	pps_params->rc_range_parameter7 =
					SWAP_TWO_BYTES(rc_range_parameters[7]);
	pps_params->rc_range_parameter8 =
					SWAP_TWO_BYTES(rc_range_parameters[8]);
	pps_params->rc_range_parameter9 =
					SWAP_TWO_BYTES(rc_range_parameters[9]);
	pps_params->rc_range_parameter10 =
					SWAP_TWO_BYTES(rc_range_parameters[10]);
	pps_params->rc_range_parameter11 =
					SWAP_TWO_BYTES(rc_range_parameters[11]);
	pps_params->rc_range_parameter12 =
					SWAP_TWO_BYTES(rc_range_parameters[12]);
	pps_params->rc_range_parameter13 =
					SWAP_TWO_BYTES(rc_range_parameters[13]);
	pps_params->rc_range_parameter14 =
					SWAP_TWO_BYTES(rc_range_parameters[14]);
}

void intel_dsc_regs_init(struct intel_encoder *encoder,
				struct intel_dsc_regs *dsc_regs, int dsc_type)
{
	switch (dsc_type) {
	case DSC_A:
		dsc_regs->dsc_picture_params0 = DSCA_PICTURE_PARAMETER_SET_0;
		dsc_regs->dsc_picture_params1 = DSCA_PICTURE_PARAMETER_SET_1;
		dsc_regs->dsc_picture_params2 = DSCA_PICTURE_PARAMETER_SET_2;
		dsc_regs->dsc_picture_params3 = DSCA_PICTURE_PARAMETER_SET_3;
		dsc_regs->dsc_picture_params4 = DSCA_PICTURE_PARAMETER_SET_4;
		dsc_regs->dsc_picture_params5 = DSCA_PICTURE_PARAMETER_SET_5;
		dsc_regs->dsc_picture_params6 = DSCA_PICTURE_PARAMETER_SET_6;
		dsc_regs->dsc_picture_params7 = DSCA_PICTURE_PARAMETER_SET_7;
		dsc_regs->dsc_picture_params8 = DSCA_PICTURE_PARAMETER_SET_8;
		dsc_regs->dsc_picture_params9 = DSCA_PICTURE_PARAMETER_SET_9;
		dsc_regs->dsc_picture_params10 = DSCA_PICTURE_PARAMETER_SET_10;
		dsc_regs->dsc_picture_params16 = DSCA_PICTURE_PARAMETER_SET_16;
		dsc_regs->dsc_rc_buff_thresh0_0 = DSCA_RC_BUF_THRESH_0_0;
		dsc_regs->dsc_rc_buff_thresh0_1 = DSCA_RC_BUF_THRESH_0_1;
		dsc_regs->dsc_rc_buff_thresh1_0 = DSCA_RC_BUF_THRESH_1_0;
		dsc_regs->dsc_rc_buff_thresh1_1 = DSCA_RC_BUF_THRESH_1_1;
		dsc_regs->dsc_rc_range0_0 = DSCA_RC_RANGE_PARAMETERS_0_0;
		dsc_regs->dsc_rc_range0_1 = DSCA_RC_RANGE_PARAMETERS_0_1;
		dsc_regs->dsc_rc_range1_0 = DSCA_RC_RANGE_PARAMETERS_1_0;
		dsc_regs->dsc_rc_range1_1 = DSCA_RC_RANGE_PARAMETERS_1_1;
		dsc_regs->dsc_rc_range2_0 = DSCA_RC_RANGE_PARAMETERS_2_0;
		dsc_regs->dsc_rc_range2_1 = DSCA_RC_RANGE_PARAMETERS_2_1;
		dsc_regs->dsc_rc_range3_0 = DSCA_RC_RANGE_PARAMETERS_3_0;
		dsc_regs->dsc_rc_range3_1 = DSCA_RC_RANGE_PARAMETERS_3_1;
		break;
	case DSC_C:
		dsc_regs->dsc_picture_params0 = DSCC_PICTURE_PARAMETER_SET_0;
		dsc_regs->dsc_picture_params1 = DSCC_PICTURE_PARAMETER_SET_1;
		dsc_regs->dsc_picture_params2 = DSCC_PICTURE_PARAMETER_SET_2;
		dsc_regs->dsc_picture_params3 = DSCC_PICTURE_PARAMETER_SET_3;
		dsc_regs->dsc_picture_params4 = DSCC_PICTURE_PARAMETER_SET_4;
		dsc_regs->dsc_picture_params5 = DSCC_PICTURE_PARAMETER_SET_5;
		dsc_regs->dsc_picture_params6 = DSCC_PICTURE_PARAMETER_SET_6;
		dsc_regs->dsc_picture_params7 = DSCC_PICTURE_PARAMETER_SET_7;
		dsc_regs->dsc_picture_params8 = DSCC_PICTURE_PARAMETER_SET_8;
		dsc_regs->dsc_picture_params9 = DSCC_PICTURE_PARAMETER_SET_9;
		dsc_regs->dsc_picture_params10 = DSCC_PICTURE_PARAMETER_SET_10;
		dsc_regs->dsc_picture_params16 = DSCC_PICTURE_PARAMETER_SET_16;
		dsc_regs->dsc_rc_buff_thresh0_0 = DSCC_RC_BUF_THRESH_0_0;
		dsc_regs->dsc_rc_buff_thresh0_1 = DSCC_RC_BUF_THRESH_0_1;
		dsc_regs->dsc_rc_buff_thresh1_0 = DSCC_RC_BUF_THRESH_1_0;
		dsc_regs->dsc_rc_buff_thresh1_1 = DSCC_RC_BUF_THRESH_1_1;
		dsc_regs->dsc_rc_range0_0 = DSCC_RC_RANGE_PARAMETERS_0_0;
		dsc_regs->dsc_rc_range0_1 = DSCC_RC_RANGE_PARAMETERS_0_1;
		dsc_regs->dsc_rc_range1_0 = DSCC_RC_RANGE_PARAMETERS_1_0;
		dsc_regs->dsc_rc_range1_1 = DSCC_RC_RANGE_PARAMETERS_1_1;
		dsc_regs->dsc_rc_range2_0 = DSCC_RC_RANGE_PARAMETERS_2_0;
		dsc_regs->dsc_rc_range2_1 = DSCC_RC_RANGE_PARAMETERS_2_1;
		dsc_regs->dsc_rc_range3_0 = DSCC_RC_RANGE_PARAMETERS_3_0;
		dsc_regs->dsc_rc_range3_1 = DSCC_RC_RANGE_PARAMETERS_3_1;
		break;
	};
}

void configure_dsc_params_for_dsc_controller(struct intel_encoder *encoder,
					struct intel_crtc_state *crtc_state,
					struct intel_dsc_regs *dsc_regs,
					int dsc_type)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dp *intel_dp = NULL;
	struct vdsc_config *vdsc_cfg  = NULL;
	unsigned long rc_range_parameters[NUM_BUF_RANGES];
	unsigned long chicken_bit_value = 0;
	unsigned int i = 0;
	int port_type = encoder->type;

	union DSC_PICTURE_PARAMETER_SET_0_BXT pps0;
	union DSC_PICTURE_PARAMETER_SET_1_BXT pps1;
	union DSC_PICTURE_PARAMETER_SET_2_BXT pps2;
	union DSC_PICTURE_PARAMETER_SET_3_BXT pps3;
	union DSC_PICTURE_PARAMETER_SET_4_BXT pps4;
	union DSC_PICTURE_PARAMETER_SET_5_BXT pps5;
	union DSC_PICTURE_PARAMETER_SET_6_BXT pps6;
	union DSC_PICTURE_PARAMETER_SET_7_BXT pps7;
	union DSC_PICTURE_PARAMETER_SET_8_BXT pps8;
	union DSC_PICTURE_PARAMETER_SET_9_BXT pps9;
	union DSC_PICTURE_PARAMETER_SET_10_BXT pps10;
	union DSC_PICTURE_PARAMETER_SET_16_BXT pps16;
	union DSC_RC_BUF_THRESH_0_BXT rc_buffer0;
	union DSC_RC_BUF_THRESH_1_BXT rc_buffer1;
	union DSC_RC_RANGE_PARAMETERS_0_BXT rc_range0;
	union DSC_RC_RANGE_PARAMETERS_1_BXT rc_range1;
	union DSC_RC_RANGE_PARAMETERS_2_BXT rc_range2;
	union DSC_RC_RANGE_PARAMETERS_3_BXT rc_range3;

	if (port_type == INTEL_OUTPUT_EDP) {

		intel_dp = enc_to_intel_dp(&encoder->base);
		vdsc_cfg = &(intel_dp->compr_params.dsc_cfg);

		/* Configure VDSC engine */
		/* PPS0 */
		pps0.dsc_version_major = vdsc_cfg->dsc_version_major;
		pps0.dsc_version_minor = vdsc_cfg->dsc_version_minor;
		pps0.bits_per_component = vdsc_cfg->bits_per_component;
		pps0.line_buf_depth = vdsc_cfg->line_buf_depth;
		pps0.block_pred_enable = vdsc_cfg->block_pred_enable;
		pps0.convert_rgb = vdsc_cfg->convert_rgb;
		pps0.enable_422 = vdsc_cfg->enable422;
		/* Since platform itself does not support VBR Enable */
		pps0.vbr_enable = 0;
		I915_WRITE(dsc_regs->dsc_picture_params0, pps0.value);

		/* PPS1 */
		pps1.bits_per_pixel = vdsc_cfg->bits_per_pixel;
		I915_WRITE(dsc_regs->dsc_picture_params1, pps1.value);

		/* PPS2 */
		pps2.pic_height = vdsc_cfg->pic_height;
		pps2.pic_width = vdsc_cfg->pic_width /
						vdsc_cfg->num_vdsc_instances;
		I915_WRITE(dsc_regs->dsc_picture_params2, pps2.value);

		/* PPS3 */
		pps3.slice_height = vdsc_cfg->slice_height;
		pps3.slice_width = vdsc_cfg->slice_width;
		I915_WRITE(dsc_regs->dsc_picture_params3, pps3.value);

		/* PPS4 */
		pps4.initial_xmit_delay = vdsc_cfg->initial_xmit_delay;
		pps4.initial_dec_delay = vdsc_cfg->initial_dec_delay;
		I915_WRITE(dsc_regs->dsc_picture_params4, pps4.value);

		/* PPS5 */
		pps5.scale_increment_interval =
					vdsc_cfg->scale_increment_interval;
		pps5.scale_decrement_interval =
					vdsc_cfg->scale_decrement_interval;
		I915_WRITE(dsc_regs->dsc_picture_params5, pps5.value);

		/* PPS6 */
		pps6.initial_scale_value = vdsc_cfg->initial_scale_value;
		pps6.first_line_bpg_offset = vdsc_cfg->first_line_bpg_Ofs;
		pps6.flatness_min_qp = vdsc_cfg->flatness_minQp;
		pps6.flatness_max_qp = vdsc_cfg->flatness_maxQp;
		I915_WRITE(dsc_regs->dsc_picture_params6, pps6.value);

		/* PPS7 */
		pps7.slice_bpg_offset = vdsc_cfg->slice_bpg_offset;
		pps7.nfl_bpg_offset = vdsc_cfg->nfl_bpg_offset;
		I915_WRITE(dsc_regs->dsc_picture_params7, pps7.value);

		/* PPS8 */
		pps8.initial_offset = vdsc_cfg->initial_offset;
		pps8.final_offset = vdsc_cfg->final_offset;
		I915_WRITE(dsc_regs->dsc_picture_params8, pps8.value);

		/* PPS9 */
		pps9.rc_edge_factor = vdsc_cfg->rc_edge_factor;
		pps9.rc_model_size = vdsc_cfg->rc_model_size;
		I915_WRITE(dsc_regs->dsc_picture_params9, pps9.value);

		/* PPS10 */
		pps10.rc_quant_incr_limit0 = vdsc_cfg->rc_quant_incr_limit0;
		pps10.rc_quant_incr_limit1 = vdsc_cfg->rc_quant_incr_limit1;
		pps10.rc_tgt_offset_hi = vdsc_cfg->rc_tgt_offset_high;
		pps10.rc_tgt_offset_lo = vdsc_cfg->rc_tgt_offset_low;
		I915_WRITE(dsc_regs->dsc_picture_params10, pps10.value);

		/* RC_Buffer 0 */
		rc_buffer0.rc_buf_thresh_0 = vdsc_cfg->rc_buf_thresh[0];
		rc_buffer0.rc_buf_thresh_1 = vdsc_cfg->rc_buf_thresh[1];
		rc_buffer0.rc_buf_thresh_2 = vdsc_cfg->rc_buf_thresh[2];
		rc_buffer0.rc_buf_thresh_3 = vdsc_cfg->rc_buf_thresh[3];
		rc_buffer0.rc_buf_thresh_4 = vdsc_cfg->rc_buf_thresh[4];
		rc_buffer0.rc_buf_thresh_5 = vdsc_cfg->rc_buf_thresh[5];
		rc_buffer0.rc_buf_thresh_6 = vdsc_cfg->rc_buf_thresh[6];
		rc_buffer0.rc_buf_thresh_7 = vdsc_cfg->rc_buf_thresh[7];
		I915_WRITE(dsc_regs->dsc_rc_buff_thresh0_0,
							rc_buffer0.value[0]);
		I915_WRITE(dsc_regs->dsc_rc_buff_thresh0_1,
							rc_buffer0.value[1]);

		/* RC_Buffer 0 */
		rc_buffer1.rc_buf_thresh_8 = vdsc_cfg->rc_buf_thresh[8];
		rc_buffer1.rc_buf_thresh_9 = vdsc_cfg->rc_buf_thresh[9];
		rc_buffer1.rc_buf_thresh_10 = vdsc_cfg->rc_buf_thresh[10];
		rc_buffer1.rc_buf_thresh_11 = vdsc_cfg->rc_buf_thresh[11];
		rc_buffer1.rc_buf_thresh_12 = vdsc_cfg->rc_buf_thresh[12];
		rc_buffer1.rc_buf_thresh_13 = vdsc_cfg->rc_buf_thresh[13];
		I915_WRITE(dsc_regs->dsc_rc_buff_thresh1_0,
							rc_buffer1.value[0]);
		I915_WRITE(dsc_regs->dsc_rc_buff_thresh1_1,
							rc_buffer1.value[1]);

		for (i = 0; i < NUM_BUF_RANGES; i++) {
			rc_range_parameters[i] = (unsigned short)(
			(vdsc_cfg->rc_range_params[i].range_bpg_offset << 10) |
			(vdsc_cfg->rc_range_params[i].range_max_qp << 5) |
			(vdsc_cfg->rc_range_params[i].range_min_qp));
		}

		/* RC Range1 */
		rc_range0.value[0] = ((rc_range_parameters[1] << 16) |
						(rc_range_parameters[0]));
		rc_range0.value[1] = ((rc_range_parameters[3] << 16) |
						(rc_range_parameters[2]));
		I915_WRITE(dsc_regs->dsc_rc_range0_0, rc_range0.value[0]);
		I915_WRITE(dsc_regs->dsc_rc_range0_1, rc_range0.value[1]);

		/* RC Range2 */
		rc_range1.value[0] = ((rc_range_parameters[5] << 16) |
						(rc_range_parameters[4]));
		rc_range1.value[1] = ((rc_range_parameters[7] << 16) |
						(rc_range_parameters[6]));
		I915_WRITE(dsc_regs->dsc_rc_range1_0, rc_range1.value[0]);
		I915_WRITE(dsc_regs->dsc_rc_range1_1, rc_range1.value[1]);

		/* RC Range3 */
		rc_range2.value[0] = ((rc_range_parameters[9] << 16) |
						(rc_range_parameters[8]));
		rc_range2.value[1] = ((rc_range_parameters[11] << 16) |
						(rc_range_parameters[10]));
		I915_WRITE(dsc_regs->dsc_rc_range2_0, rc_range2.value[0]);
		I915_WRITE(dsc_regs->dsc_rc_range2_1, rc_range2.value[1]);

		/* RC Range4 */
		rc_range3.value[0] = ((rc_range_parameters[13] << 16) |
						(rc_range_parameters[12]));
		rc_range3.value[1] = rc_range_parameters[14];
		I915_WRITE(dsc_regs->dsc_rc_range3_0, rc_range3.value[0]);
		I915_WRITE(dsc_regs->dsc_rc_range3_1, rc_range3.value[1]);

		/* PPS16 */
		pps16.slice_chunk_size = vdsc_cfg->chunk_size;
		pps16.slice_per_line =
			(vdsc_cfg->pic_width / vdsc_cfg->num_vdsc_instances) /
							vdsc_cfg->slice_width;
		pps16.slice_row_per_frame =
				vdsc_cfg->pic_height / vdsc_cfg->slice_height;
		I915_WRITE(dsc_regs->dsc_picture_params16, pps16.value);

		chicken_bit_value = I915_READ(DSC_CHICKEN_1_A);
		I915_WRITE(DSC_CHICKEN_1_A, 0x80000000);
	}
}

void enable_pps_dip(struct intel_encoder *encoder,
					struct intel_dsc_regs *dsc_regs)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	int type = encoder->port;
	i915_reg_t dip_ctrl_reg;
	unsigned int value = 0;

	if (type == INTEL_OUTPUT_EDP || type == INTEL_OUTPUT_DP) {
		dip_ctrl_reg = dsc_regs->dip_ctrl_reg;
		value = I915_READ(dip_ctrl_reg);
		value |= VDIP_ENABLE_PPS;
		I915_WRITE(dip_ctrl_reg, value);
	}
}

void write_dip(struct intel_encoder *encoder, unsigned char *dip_data,
						unsigned char dip_size,
						struct intel_dsc_regs *dsc_regs)
{
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	unsigned int max_offset;
	unsigned char count = 0;
	unsigned char i = 0;
	unsigned char data_to_write = 0;
	unsigned char offset = 0;
	unsigned long remaining_buffer = 0;
	unsigned int video_dip_pps_data = 0;
	unsigned int payload_data_reg;

	/*
	 * 33*4 = 132
	 * 4 byte SDP header + 128 byte PPS data
	 */
	max_offset = 33;
	payload_data_reg = dsc_regs->dip_pps_data_ctrl_reg;

	if (dip_data) {
		remaining_buffer = max_offset * 4;
		data_to_write = dip_size;

		while (remaining_buffer > 0 && offset < max_offset) {
			if (data_to_write >= 4) {
				video_dip_pps_data = (dip_data[count] |
						(dip_data[count + 1] << 8) |
						(dip_data[count + 2] << 16) |
						(dip_data[count + 3] << 24));
				data_to_write -= 4;
				count += 4;
				I915_WRITE(_MMIO(payload_data_reg),
							video_dip_pps_data);
			} else {
				unsigned char buffer[4];

				memset(&buffer[0], 0,
						4 * sizeof(unsigned char));
				for (i = 0; i < data_to_write; i++)
					buffer[i] = dip_data[count++];
				video_dip_pps_data = (buffer[0] |
					(buffer[1] << 8) |
					(buffer[2] << 16) | (buffer[3] << 24));
				data_to_write = 0;
				I915_WRITE(_MMIO(payload_data_reg),
							video_dip_pps_data);
			}
			payload_data_reg += 0x4;
			remaining_buffer -= 4;
			offset++;
		}
	}
}

void send_pps_sdp_to_sink(struct intel_encoder *encoder, int pipe,
			struct picture_parameters_set *pps_params,
			struct intel_dsc_regs *dsc_regs)
{
	union pps_sdp sdp;
	unsigned char payload_size = 0;

	sdp.secondary_data_packet_header.sdp_id = 0;
	sdp.secondary_data_packet_header.sdp_type = 0x10;
	sdp.secondary_data_packet_header.sdp_byte1 = 0x7F;
	sdp.secondary_data_packet_header.sdp_byte2 = 0x0;
	sdp.pps_payload = *pps_params;

	payload_size = SDP_HEADER_SIZE + PPS_PAYLOAD_SIZE;
	write_dip(encoder, (unsigned char *)&sdp, payload_size, dsc_regs);
}

void intel_dsc_enable(struct intel_encoder *encoder,
				struct intel_crtc_state *pipe_config)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&encoder->base);
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct picture_parameters_set pps_params;
	struct intel_dsc_regs dsc_regs;
	struct drm_crtc *crtc = pipe_config->base.crtc;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int pipe = intel_crtc->pipe;
	int dsc_type1;
	int dsc_type2;
	int type = encoder->type;
	unsigned int dss_ctrl1_value = 0;
	unsigned int dss_ctrl2_value = 0;

	if ((INTEL_GEN(dev_priv) < 9) ||
				!intel_dp->compr_params.compression_support)
		return;
	/* TO DO: configure DSC params and program source regs */

	if (type == INTEL_OUTPUT_EDP) {
		dsc_regs.dss_ctrl1_reg = DSS_CONTROL1;
		dsc_regs.dss_ctrl2_reg = DSS_CONTROL2;
		dsc_regs.dip_ctrl_reg = VIDEO_DIP_CTL_EDP;
		dsc_regs.dip_pps_data_ctrl_reg = VIDEO_DIP_PPS_DATA_EDP_REG;
		dsc_type1 = DSC_A;
		dsc_type2 = DSC_C;
	} else if (type == INTEL_OUTPUT_DP) {
		switch (pipe) {
		case PIPE_A:
			dsc_regs.dss_ctrl1_reg = PIPE_DSS_CTL1_PB;
			dsc_regs.dss_ctrl2_reg = PIPE_DSS_CTL2_PB;
			dsc_regs.dip_ctrl_reg = VIDEO_DIP_CTL_A;
			dsc_regs.dip_pps_data_ctrl_reg =
						VIDEO_DIP_DRM_DATA_TRANSA_REG;
			dsc_type1 = PIPEA_DSC_0;
			dsc_type2 = PIPEA_DSC_1;
			break;
		case PIPE_B:
			dsc_regs.dss_ctrl1_reg = PIPE_DSS_CTL1_PC;
			dsc_regs.dss_ctrl2_reg = PIPE_DSS_CTL2_PC;
			dsc_regs.dip_ctrl_reg = VIDEO_DIP_CTL_B;
			dsc_regs.dip_pps_data_ctrl_reg =
						VIDEO_DIP_DRM_DATA_TRANSB_REG;
			dsc_type1 = PIPEB_DSC_0;
			dsc_type2 = PIPEB_DSC_1;
			break;
		default:
			return;
		}
	} else {
		DRM_ERROR("Func:%s Unsupported port:%d\n", __func__, type);
	}

	intel_dsc_regs_init(encoder, &dsc_regs, dsc_type1);
	configure_dsc_params_for_dsc_controller(encoder, pipe_config,
							&dsc_regs, dsc_type1);
	if (intel_dp->compr_params.dsc_cfg.num_vdsc_instances != 1) {
		intel_dsc_regs_init(encoder, &dsc_regs, dsc_type2);
		configure_dsc_params_for_dsc_controller(encoder, pipe_config,
							&dsc_regs, dsc_type2);
	}
	populate_pps_sdp_for_sink(encoder, pipe_config, &pps_params);

	send_pps_sdp_to_sink(encoder, pipe, &pps_params, &dsc_regs);

	enable_pps_dip(encoder, &dsc_regs);

	dss_ctrl1_value = I915_READ(dsc_regs.dss_ctrl1_reg);
	dss_ctrl2_value = I915_READ(dsc_regs.dss_ctrl2_reg);
	if (type == INTEL_OUTPUT_EDP || type == INTEL_OUTPUT_DP) {
		 /*
		  * Enable joiner only if we are using both the VDSC engines
		  * To check if splitters gets enabled by default in HW
		  * if joiner is enabled
		  */
		if (intel_dp->compr_params.dsc_cfg.num_vdsc_instances != 1)
			dss_ctrl1_value |= JOINER_ENABLE | SPLITTER_ENABLE;

		I915_WRITE(dsc_regs.dss_ctrl1_reg, dss_ctrl1_value);

		dss_ctrl2_value |= LEFT_BRANCH_VDSC_ENABLE;
		if (intel_dp->compr_params.dsc_cfg.num_vdsc_instances != 1)
			dss_ctrl2_value |= RIGHT_BRANCH_VDSC_ENABLE;

		I915_WRITE(dsc_regs.dss_ctrl2_reg, dss_ctrl2_value);
	}
}

void intel_dsc_disable(struct intel_encoder *encoder,
				struct intel_crtc_state *pipe_config)
{
	struct intel_dp *intel_dp = enc_to_intel_dp(&encoder->base);
	struct drm_i915_private *dev_priv = to_i915(encoder->base.dev);
	struct intel_dsc_regs dsc_regs;
	struct drm_crtc *crtc = pipe_config->base.crtc;
	struct intel_crtc *intel_crtc = to_intel_crtc(crtc);
	int pipe = intel_crtc->pipe;
	int type = encoder->type;
	unsigned int dss_ctrl1_value = 0;
	unsigned int dss_ctrl2_value = 0;

	if ((INTEL_GEN(dev_priv) < 9) ||
				!intel_dp->compr_params.compression_support)
		return;

	if (type == INTEL_OUTPUT_EDP) {
		dsc_regs.dss_ctrl1_reg = DSS_CONTROL1;
		dsc_regs.dss_ctrl2_reg = DSS_CONTROL2;
	} else if (type == INTEL_OUTPUT_DP) {
		switch (pipe) {
		case PIPE_A:
			dsc_regs.dss_ctrl1_reg = PIPE_DSS_CTL1_PB;
			dsc_regs.dss_ctrl2_reg = PIPE_DSS_CTL2_PB;
			break;
		case PIPE_B:
			dsc_regs.dss_ctrl1_reg = PIPE_DSS_CTL1_PC;
			dsc_regs.dss_ctrl2_reg = PIPE_DSS_CTL2_PC;
			break;
		default:
			return;
		}
	} else {
		DRM_ERROR("Func:%s Unsupported port:%d\n", __func__, type);
	}

	dss_ctrl1_value = I915_READ(dsc_regs.dss_ctrl1_reg);
	dss_ctrl2_value = I915_READ(dsc_regs.dss_ctrl2_reg);

	if ((dss_ctrl2_value & LEFT_BRANCH_VDSC_ENABLE) ||
		(dss_ctrl2_value & RIGHT_BRANCH_VDSC_ENABLE))
		dss_ctrl2_value &= LEFT_BRANCH_VDSC_DISABLE &
						RIGHT_BRANCH_VDSC_DISABLE;
	I915_WRITE(dsc_regs.dss_ctrl2_reg, dss_ctrl2_value);

	if (dss_ctrl1_value & JOINER_ENABLE)
		dss_ctrl1_value &= JOINER_DISABLE;
	I915_WRITE(dsc_regs.dss_ctrl1_reg, dss_ctrl1_value);
}
