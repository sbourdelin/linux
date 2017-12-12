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

#include "intel_guc_wopcm.h"
#include "i915_drv.h"

/*
 * intel_guc_wopcm_size() - Get the size of GuC WOPCM.
 * @guc: intel guc.
 *
 * Get the platform specific GuC WOPCM size.
 *
 * Return: size of the GuC WOPCM.
 */
u32 intel_guc_wopcm_size(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_i915(guc);

	u32 wopcm_size = GUC_WOPCM_TOP;

	/* On BXT, the top of WOPCM is reserved for RC6 context */
	if (IS_GEN9_LP(i915))
		wopcm_size -= BXT_GUC_WOPCM_RC6_RESERVED;

	return wopcm_size;
}
