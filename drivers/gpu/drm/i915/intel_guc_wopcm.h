/*
 * Copyright © 2017 Intel Corporation
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

#ifndef _INTEL_GUC_WOPCM_H_
#define _INTEL_GUC_WOPCM_H_

#include <linux/types.h>

struct intel_guc;

/* Default WOPCM size 1MB */
#define WOPCM_DEFAULT_SIZE		(0x1 << 20)
/* Reserved WOPCM size 16KB */
#define WOPCM_RESERVED_SIZE		(0x4000)
/* GUC WOPCM Offset need to be 16KB aligned */
#define WOPCM_OFFSET_ALIGNMENT		(0x4000)
/* 8KB stack reserved for GuC FW*/
#define GUC_WOPCM_STACK_RESERVED	(0x2000)
/* 24KB WOPCM reserved for RC6 CTX on BXT */
#define BXT_WOPCM_RC6_RESERVED		(0x6000)

#define GEN9_GUC_WOPCM_DELTA		4
#define GEN9_GUC_WOPCM_OFFSET		(0x24000)

struct intel_guc_wopcm {
	u32 offset;
	u32 size;
	u32 top;
	bool valid;
};

/*
 * intel_guc_wopcm_init_early() - Early initialization of the GuC WOPCM.
 * @wopcm: GuC WOPCM.
 *
 * Setup the GuC WOPCM top to the top of the overall WOPCM. This will guarantee
 * that the allocation of the GuC accessible objects won't fall into WOPCM when
 * GuC partition isn't present.
 *
 */
static inline void intel_guc_wopcm_init_early(struct intel_guc_wopcm *wopcm)
{
	wopcm->top = WOPCM_DEFAULT_SIZE;
}

int intel_guc_wopcm_init(struct intel_guc *guc, u32 guc_size, u32 huc_size);

#endif
