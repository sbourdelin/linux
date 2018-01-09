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

static inline u32 guc_reserved_wopcm_size(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_i915(guc);

	/* On BXT, the top of WOPCM is reserved for RC6 context */
	if (IS_GEN9_LP(i915))
		return BXT_WOPCM_RC6_RESERVED;

	if (IS_GEN10(i915))
		return CNL_WOPCM_RESERVED;

	return 0;
}

/*
 * On Gen9 & CNL A0, hardware requires GuC size to be larger than or equal to
 * HuC kernel size.
 */
static inline int wopcm_huc_size_check(struct drm_i915_private *i915)
{
	struct intel_guc_wopcm *wopcm = &i915->guc.wopcm;
	u32 huc_size = intel_uc_fw_get_size(&i915->huc.fw);


	if (unlikely(wopcm->size - GUC_WOPCM_RESERVED < huc_size))
		return -E2BIG;

	return 0;
}

static inline int gen9_wocpm_size_check(struct drm_i915_private *i915)
{
	struct intel_guc_wopcm *wopcm = &i915->guc.wopcm;
	u32 wopcm_base;
	u32 delta;

	/*
	 * Check hardware restriction on Gen9
	 * GuC WOPCM size is at least 4 bytes larger than GuC WOPCM base due
	 * to hardware limitation on Gen9.
	 */
	wopcm_base = wopcm->offset + GEN9_GUC_WOPCM_OFFSET;
	if (unlikely(wopcm_base > wopcm->size))
		return -E2BIG;

	delta = wopcm->size - wopcm_base;
	if (unlikely(delta < GEN9_GUC_WOPCM_DELTA))
		return -E2BIG;

	return wopcm_huc_size_check(i915);
}

static inline int guc_wopcm_size_check(struct intel_guc *guc)
{
	struct drm_i915_private *i915 = guc_to_i915(guc);

	if (IS_GEN9(i915))
		return gen9_wocpm_size_check(i915);

	if (IS_CNL_REVID(i915, CNL_REVID_A0, CNL_REVID_A0))
		return wopcm_huc_size_check(i915);

	return 0;
}

/*
 * intel_guc_wopcm_init() - Initialize the GuC WOPCM partition.
 * @guc: intel guc.
 * @guc_fw_size: size of GuC firmware.
 * @huc_fw_size: size of HuC firmware.
 *
 * This function tries to initialize the WOPCM partition based on HuC firmware
 * size and the reserved WOPCM memory size.
 *
 * Return: 0 on success, non-zero error code on failure.
 */
int intel_guc_wopcm_init(struct intel_guc *guc, u32 guc_fw_size,
			 u32 huc_fw_size)
{
	u32 reserved = guc_reserved_wopcm_size(guc);
	u32 offset, size, top;
	int err;

	if (guc->wopcm.valid)
		return 0;

	if (!guc_fw_size)
		return -EINVAL;

	if (reserved >= WOPCM_DEFAULT_SIZE)
		return -E2BIG;

	offset = huc_fw_size + WOPCM_RESERVED_SIZE;
	if (offset >= WOPCM_DEFAULT_SIZE)
		return -E2BIG;

	/* Hardware requires GuC WOPCM offset needs to be 16K aligned. */
	offset = ALIGN(offset, WOPCM_OFFSET_ALIGNMENT);
	if ((offset + reserved) >= WOPCM_DEFAULT_SIZE)
		return -E2BIG;

	top = WOPCM_DEFAULT_SIZE - offset;
	size = top - reserved;

	/* GuC WOPCM size must be 4K aligned. */
	size &= GUC_WOPCM_SIZE_MASK;

	/*
	 * GuC size needs to be less than or equal to GuC WOPCM size.
	 * Need extra 8K stack for GuC.
	 */
	if ((guc_fw_size + GUC_WOPCM_STACK_RESERVED) > size)
		return -E2BIG;

	guc->wopcm.offset = offset;
	guc->wopcm.size = size;
	guc->wopcm.top = top;

	/* Check platform specific restrictions */
	err = guc_wopcm_size_check(guc);
	if (err)
		return err;

	guc->wopcm.valid = true;

	DRM_DEBUG_DRIVER("GuC WOPCM offset %dKB, size %dKB, top %dKB\n",
			 offset >> 10, size >> 10, top >> 10);

	return 0;
}
