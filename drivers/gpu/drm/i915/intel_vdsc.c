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

	/* The PPS structure is stored as per our hardware registers which
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
